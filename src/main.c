#include "ffmpeg_decoder.h"
#include "h264.h"
#include "hid.h"
#include "protocol.h"
#include "rtp_client.h"

#include <freerdp/channels/channels.h>
#include <freerdp/channels/drdynvc.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/interleaved.h>
#include <freerdp/codec/nsc.h>
#include <freerdp/codec/progressive.h>
#include <freerdp/codec/rfx.h>
#include <freerdp/freerdp.h>
#include <freerdp/input.h>
#include <freerdp/listener.h>
#include <freerdp/peer.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/settings.h>
#include <freerdp/update.h>

#include <winpr/crt.h>
#include <winpr/ssl.h>
#include <winpr/sysinfo.h>
#include <winpr/synch.h>
#include <winpr/thread.h>
#include <winpr/stream.h>
#include <winpr/wtsapi.h>
#include <winpr/winsock.h>

#include <dlfcn.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef WINPR_C_ARRAY_INIT
#define WINPR_C_ARRAY_INIT \
	{ \
		0 \
	}
#endif

#define TAG "nanokvm-rdp-gateway"
#define DEFAULT_WIDTH 1920U
#define DEFAULT_HEIGHT 1080U
#define DEFAULT_BITRATE 8000U
#define MAX_EVENT_HANDLES 32U
#define BITMAP_QUEUE_CAPACITY 16U
#define BITMAP_FRAME_INTERVAL_MS 10U
#define BITMAP_MAX_QUEUE_AGE_MS 100U
#define CLASSIC_TILE_WIDTH 64U
#define CLASSIC_TILE_HEIGHT 64U
#define CLASSIC_TILE_MAX_ENCODED (CLASSIC_TILE_WIDTH * CLASSIC_TILE_HEIGHT * 4U)
#define CLASSIC_BITMAP_BATCH 1U
#define CLASSIC_MAX_UPDATE_SIZE (32U * 1024U)
#define CLASSIC_PIXEL_DIFF_THRESHOLD 10U
#define CLASSIC_CHANGED_PIXEL_THRESHOLD 8U
#define HEARTBEAT_INTERVAL_MS 1000U
#define HEARTBEAT_TIMEOUT_MS 5000U
#define STATS_LOG_INTERVAL_MS 5000U

typedef struct
{
	const char* bind_address;
	uint16_t port;
	const char* certificate;
	const char* private_key;
	const char* keyboard;
	const char* mouse;
	const char* touch;
	uint16_t width;
	uint16_t height;
	uint16_t bitrate;
	uint16_t control_port;
	uint16_t video_port;
	bool swap_alt_command;
	bool right_alt_as_capslock;
	bool direct_gfx;
} ServerConfig;

typedef struct Server Server;
typedef struct Client Client;

struct Server
{
	ServerConfig config;
	CRITICAL_SECTION lock;
	CRITICAL_SECTION control_lock;
	int control_listener;
	int control_fd;
	bool stream_requested;
	uint64_t last_agent_activity_at;
	uint64_t last_ping_at;
	uint64_t last_stats_log_at;
	uint32_t agent_sent_packets;
	uint32_t agent_dropped_packets;
	uint32_t agent_capture_frames;
	uint32_t agent_dropped_frames;
	Client* active;
};

struct Client
{
	rdpContext context;
	Server* server;
	freerdp_peer* peer;
	HANDLE vcm;
	RdpgfxServerContext* gfx;
	HANDLE video_thread;
	HANDLE bitmap_ready_event;
	RFX_CONTEXT* rfx;
	NSC_CONTEXT* nsc;
	PROGRESSIVE_CONTEXT* progressive;
	BITMAP_INTERLEAVED_CONTEXT* interleaved;
	wStream* bitmap_stream;
	CRITICAL_SECTION lock;
	uint8_t* pending_bitmap;
	size_t pending_bitmap_length;
	uint8_t* bitmap_queue[BITMAP_QUEUE_CAPACITY];
	size_t bitmap_queue_lengths[BITMAP_QUEUE_CAPACITY];
	uint64_t bitmap_queue_queued_at[BITMAP_QUEUE_CAPACITY];
	uint8_t bitmap_queue_head;
	uint8_t bitmap_queue_count;
	uint8_t* previous_bitmap;
	size_t previous_bitmap_length;
	uint8_t* classic_encoded;
	bool previous_bitmap_valid;
	bool bitmap_pending;
	HidState hid;
	bool stopping;
	bool owns_active_client;
	bool direct_gfx_active;
	bool bitmap_fallback_active;
	bool gfx_ready;
	bool gfx_opened;
	bool inflight;
	bool need_idr;
	uint64_t gfx_wait_started_at;
	uint64_t gfx_opened_at;
	uint32_t inflight_frame_id;
	uint32_t next_frame_id;
	uint64_t sent_at;
	uint8_t* sps;
	size_t sps_length;
	uint8_t* pps;
	size_t pps_length;
	bool bitmap_uses_rfx;
	bool gfx_uses_progressive;
	uint16_t render_width;
	uint16_t render_height;
	uint32_t bitmap_frames;
	uint32_t decoded_frames;
	uint32_t bitmap_queued_frames;
	uint32_t bitmap_queue_drops;
	uint32_t bitmap_stale_drops;
	uint32_t bitmap_flushes;
	uint32_t bitmap_empty_flushes;
	uint64_t classic_tiles_sent;
	uint64_t classic_bytes_sent;
	uint32_t rtp_nals;
	uint32_t rtp_access_units;
	uint32_t rtp_idr_units;
	uint32_t rtp_p_units;
	uint64_t bitmap_last_send_started_at;
	bool keyboard_input_logged;
	bool pointer_input_logged;
	bool wheel_input_logged;
	uint8_t relative_buttons;
	uint64_t last_rtp_received_at;
	uint64_t last_decode_latency_ms;
	uint64_t last_rdp_send_ms;
};

static volatile sig_atomic_t stop_requested = 0;

static uint64_t monotonic_milliseconds(void);
static DWORD WINAPI video_thread(LPVOID argument);
static DWORD WINAPI bitmap_video_thread(LPVOID argument);

static void log_message(const char* level, const char* message)
{
	(void)fprintf(stderr, "%s: %s: %s\n", TAG, level, message);
}

static bool server_send_control(Server* server, uint8_t type, const void* payload, uint16_t length)
{
	bool sent = false;
	EnterCriticalSection(&server->control_lock);
	if (server->control_fd >= 0)
		sent = protocol_send(server->control_fd, type, payload, length);
	LeaveCriticalSection(&server->control_lock);
	return sent;
}

static bool server_set_stream_requested(Server* server, bool requested)
{
	bool sent = false;
	EnterCriticalSection(&server->control_lock);
	server->stream_requested = requested;
	if (server->control_fd >= 0)
		sent = protocol_send(server->control_fd,
		                     requested ? NANOKVM_CONTROL_START_STREAM : NANOKVM_CONTROL_STOP_STREAM,
		                     NULL, 0);
	LeaveCriticalSection(&server->control_lock);
	log_message(sent ? "INFO" : "ERROR", requested ? "NanoKVM agent에 START_STREAM 전송"
	                                                : "NanoKVM agent에 STOP_STREAM 전송");
	return sent;
}

static void on_signal(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static DWORD WINAPI control_thread(LPVOID argument)
{
	Server* server = (Server*)argument;
	while (!stop_requested)
	{
		struct sockaddr_in address = { 0 };
		socklen_t length = sizeof(address);
		const int fd = accept(server->control_listener, (struct sockaddr*)&address, &length);
		if (fd < 0)
			continue;
		int enabled = 1;
		(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
		NanokvmControlMessage hello = { 0 };
		if (!protocol_receive(fd, &hello) || hello.type != NANOKVM_CONTROL_HELLO || hello.length != 8)
		{
			(void)close(fd);
			continue;
		}
		EnterCriticalSection(&server->control_lock);
		if (server->control_fd >= 0)
			(void)close(server->control_fd);
		server->control_fd = fd;
		server->last_agent_activity_at = monotonic_milliseconds();
		server->last_ping_at = server->last_agent_activity_at;
		const bool start = server->stream_requested;
		LeaveCriticalSection(&server->control_lock);
	if (start && !server_send_control(server, NANOKVM_CONTROL_START_STREAM, NULL, 0))
	{
		log_message("ERROR", "재연결 NanoKVM agent에 START_STREAM을 보낼 수 없습니다");
	}
		log_message("INFO", "NanoKVM agent control 연결 수락");
		for (;;)
		{
			NanokvmControlMessage message = { 0 };
			if (!protocol_receive(fd, &message))
				break;
			EnterCriticalSection(&server->control_lock);
			server->last_agent_activity_at = monotonic_milliseconds();
			if (message.type == NANOKVM_CONTROL_STATS &&
			    message.length == NANOKVM_STATS_PAYLOAD_SIZE)
			{
				server->agent_sent_packets = protocol_read_u32(message.payload);
				server->agent_dropped_packets = protocol_read_u32(message.payload + 4);
				server->agent_capture_frames = protocol_read_u32(message.payload + 8);
				server->agent_dropped_frames = protocol_read_u32(message.payload + 12);
			}
			LeaveCriticalSection(&server->control_lock);
			if (message.type == NANOKVM_CONTROL_PING)
				(void)server_send_control(server, NANOKVM_CONTROL_PONG, NULL, 0);
		}
		EnterCriticalSection(&server->control_lock);
		if (server->control_fd == fd)
			server->control_fd = -1;
		LeaveCriticalSection(&server->control_lock);
		(void)close(fd);
		log_message("WARN", "NanoKVM agent control 연결 종료");
	}
	return 0;
}

static int open_control_listener(const char* host, uint16_t port)
{
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	int enabled = 1;
	(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
	struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(port) };
	if (inet_pton(AF_INET, host, &address.sin_addr) != 1 ||
	    bind(fd, (const struct sockaddr*)&address, sizeof(address)) != 0 || listen(fd, 1) != 0)
	{
		(void)close(fd);
		return -1;
	}
	return fd;
}

static uint64_t monotonic_milliseconds(void)
{
	return (uint64_t)GetTickCount64();
}

static void server_heartbeat(Server* server)
{
	const uint64_t now = monotonic_milliseconds();
	bool send_ping = false;
	bool timeout = false;
	freerdp_peer* stale_peer = NULL;
	EnterCriticalSection(&server->control_lock);
	if (server->control_fd >= 0)
	{
		timeout = now - server->last_agent_activity_at > HEARTBEAT_TIMEOUT_MS;
		send_ping = now - server->last_ping_at >= HEARTBEAT_INTERVAL_MS;
		if (send_ping)
			server->last_ping_at = now;
		if (timeout)
			(void)shutdown(server->control_fd, SHUT_RDWR);
	}
	LeaveCriticalSection(&server->control_lock);
	if (timeout)
	{
		/* agent가 멈추면 비디오도 멈추므로 기존 RDP client 세션도 의미가 없다.
		 * active client를 강제 종료하지 않으면 single-client 슬롯이 점유된 채
		 * 남아 이후 재접속이 모두 listener에서 거부된다. */
		EnterCriticalSection(&server->lock);
		if (server->active && server->active->peer)
		{
			stale_peer = server->active->peer;
			/* 중복 disconnect 방지: active는 client_context_free가 정리한다. */
			server->active = NULL;
		}
		LeaveCriticalSection(&server->lock);
		if (stale_peer && stale_peer->Disconnect)
		{
			stale_peer->Disconnect(stale_peer);
			log_message("WARN", "NanoKVM agent heartbeat timeout; active RDP client 연결을 종료합니다");
		}
		else
			log_message("WARN", "NanoKVM agent heartbeat timeout; control 연결을 종료합니다");
		(void)server_set_stream_requested(server, false);
	}
	if (send_ping && !timeout)
		(void)server_send_control(server, NANOKVM_CONTROL_PING, NULL, 0);
	if (now - server->last_stats_log_at < STATS_LOG_INTERVAL_MS)
		return;
	server->last_stats_log_at = now;
	struct rusage usage = { 0 };
	if (getrusage(RUSAGE_SELF, &usage) == 0)
	{
		uint32_t bitmap_frames = 0;
		uint32_t decoded_frames = 0;
		uint32_t bitmap_queued_frames = 0;
		uint32_t bitmap_queue_drops = 0;
		uint32_t bitmap_stale_drops = 0;
		uint32_t bitmap_flushes = 0;
		uint32_t bitmap_empty_flushes = 0;
		uint64_t classic_tiles_sent = 0;
		uint64_t classic_bytes_sent = 0;
		uint32_t rtp_nals = 0;
		uint32_t rtp_access_units = 0;
		uint32_t rtp_idr_units = 0;
		uint32_t rtp_p_units = 0;
		bool bitmap_pending = false;
		uint64_t rdp_send_ms = 0;
		EnterCriticalSection(&server->lock);
		Client* active = server->active;
		if (active)
		{
			EnterCriticalSection(&active->lock);
			bitmap_frames = active->bitmap_frames;
			decoded_frames = active->decoded_frames;
			bitmap_queued_frames = active->bitmap_queued_frames;
			bitmap_queue_drops = active->bitmap_queue_drops;
			bitmap_stale_drops = active->bitmap_stale_drops;
			bitmap_flushes = active->bitmap_flushes;
			bitmap_empty_flushes = active->bitmap_empty_flushes;
			classic_tiles_sent = active->classic_tiles_sent;
			classic_bytes_sent = active->classic_bytes_sent;
			rtp_nals = active->rtp_nals;
			rtp_access_units = active->rtp_access_units;
			rtp_idr_units = active->rtp_idr_units;
			rtp_p_units = active->rtp_p_units;
			bitmap_pending = active->bitmap_queue_count > 0;
			rdp_send_ms = active->last_rdp_send_ms;
			LeaveCriticalSection(&active->lock);
		}
		LeaveCriticalSection(&server->lock);
		char message[512] = { 0 };
		(void)snprintf(message, sizeof(message),
			               "STATS agent packets=%u dropped=%u frames=%u dropped_frames=%u rtp_nals=%u au=%u idr=%u p=%u decoded=%u queued=%u queue_drop=%u stale_drop=%u flush=%u empty_flush=%u rdp_frames=%u classic_tiles=%llu classic_bytes=%llu queue=%u gateway_rss=%ld decode_ms=%llu rdp_send_ms=%llu",
			               server->agent_sent_packets, server->agent_dropped_packets,
			               server->agent_capture_frames, server->agent_dropped_frames,
			               rtp_nals, rtp_access_units, rtp_idr_units, rtp_p_units, decoded_frames,
			               bitmap_queued_frames, bitmap_queue_drops, bitmap_stale_drops, bitmap_flushes,
			               bitmap_empty_flushes,
			               bitmap_frames, (unsigned long long)classic_tiles_sent,
			               (unsigned long long)classic_bytes_sent, bitmap_pending ? 1U : 0U,
		               usage.ru_maxrss, (unsigned long long)(server->active ?
		               server->active->last_decode_latency_ms : 0),
		               (unsigned long long)rdp_send_ms);
		log_message("INFO", message);
	}
}

static bool copy_bytes(uint8_t** destination, size_t* destination_length, const uint8_t* source,
	                      size_t source_length)
{
	uint8_t* next = NULL;
	if (source_length > 0)
	{
		next = malloc(source_length);
		if (!next)
			return false;
		memcpy(next, source, source_length);
	}
	free(*destination);
	*destination = next;
	*destination_length = source_length;
	return true;
}

static bool client_should_stop(Client* client)
{
	bool stopping = false;
	EnterCriticalSection(&client->lock);
	stopping = client->stopping;
	LeaveCriticalSection(&client->lock);
	return stopping;
}

static void client_stop(Client* client)
{
	EnterCriticalSection(&client->lock);
	client->stopping = true;
	LeaveCriticalSection(&client->lock);
}

static bool client_cap_supports_avc420(const RDPGFX_CAPSET* cap)
{
	if (cap->version == RDPGFX_CAPVERSION_81)
		return (cap->flags & RDPGFX_CAPS_FLAG_AVC420_ENABLED) != 0;
	return cap->version >= RDPGFX_CAPVERSION_10 &&
	       (cap->flags & RDPGFX_CAPS_FLAG_AVC_DISABLED) == 0;
}

static bool client_select_gfx_cap(const RDPGFX_CAPS_ADVERTISE_PDU* advertise,
	                              RDPGFX_CAPSET* selected, bool* use_avc420)
{
	const UINT32 preferred_versions[] = {
		RDPGFX_CAPVERSION_107, RDPGFX_CAPVERSION_106, RDPGFX_CAPVERSION_106_ERR,
		RDPGFX_CAPVERSION_105, RDPGFX_CAPVERSION_104, RDPGFX_CAPVERSION_103,
		RDPGFX_CAPVERSION_102, RDPGFX_CAPVERSION_101, RDPGFX_CAPVERSION_10,
		RDPGFX_CAPVERSION_81, RDPGFX_CAPVERSION_8
	};

	for (size_t pass = 0; pass < 2; pass++)
	{
		for (size_t version = 0; version < ARRAYSIZE(preferred_versions); version++)
		{
			for (UINT32 index = 0; index < advertise->capsSetCount; index++)
			{
				const RDPGFX_CAPSET* current = &advertise->capsSets[index];
				if (current->version != preferred_versions[version])
					continue;
				const bool avc420 = client_cap_supports_avc420(current);
				if (pass == 0 && !avc420)
					continue;
				*selected = *current;
				*use_avc420 = avc420;
				return true;
			}
		}
	}
	return false;
}

static UINT on_gfx_caps_advertise(RdpgfxServerContext* gfx,
	                              const RDPGFX_CAPS_ADVERTISE_PDU* advertise)
{
	Client* client = (Client*)gfx->custom;
	RDPGFX_CAPSET selected = WINPR_C_ARRAY_INIT;
	RDPGFX_CAPS_CONFIRM_PDU confirm = WINPR_C_ARRAY_INIT;
	RDPGFX_CREATE_SURFACE_PDU surface = WINPR_C_ARRAY_INIT;
	RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU map = WINPR_C_ARRAY_INIT;
	bool use_avc420 = false;
	UINT error = CHANNEL_RC_OK;

	if (!client->direct_gfx_active || client->bitmap_fallback_active)
		return ERROR_NOT_SUPPORTED;
	for (UINT32 index = 0; index < advertise->capsSetCount; index++)
	{
		char message[160];
		(void)snprintf(message, sizeof(message),
		               "RDPGFX client capability[%u]: version=0x%08x flags=0x%08x",
		               index, advertise->capsSets[index].version,
		               advertise->capsSets[index].flags);
		log_message("INFO", message);
	}
	if (!client_select_gfx_cap(advertise, &selected, &use_avc420))
	{
		log_message("ERROR", "client가 지원 가능한 RDPGFX capability를 광고하지 않았습니다");
		client_stop(client);
		return CHANNEL_RC_UNSUPPORTED_VERSION;
	}

	confirm.capsSet = &selected;
	if (!gfx->CapsConfirm || (error = gfx->CapsConfirm(gfx, &confirm)) != CHANNEL_RC_OK)
		return error ? error : ERROR_INTERNAL_ERROR;

	surface.surfaceId = 1;
	surface.width = client->server->config.width;
	surface.height = client->server->config.height;
	surface.pixelFormat = GFX_PIXEL_FORMAT_XRGB_8888;
	if (!gfx->CreateSurface || (error = gfx->CreateSurface(gfx, &surface)) != CHANNEL_RC_OK)
		return error ? error : ERROR_INTERNAL_ERROR;

	map.surfaceId = surface.surfaceId;
	map.outputOriginX = 0;
	map.outputOriginY = 0;
	map.reserved = 0;
	if (!gfx->MapSurfaceToOutput || (error = gfx->MapSurfaceToOutput(gfx, &map)) != CHANNEL_RC_OK)
		return error ? error : ERROR_INTERNAL_ERROR;
	if (!use_avc420)
	{
		client->progressive = progressive_context_new_ex(
		    TRUE, freerdp_settings_get_uint32(client->context.settings, FreeRDP_ThreadingFlags));
		if (!client->progressive || !progressive_context_reset(client->progressive))
			return ERROR_INTERNAL_ERROR;
	}

	EnterCriticalSection(&client->lock);
	client->gfx_ready = true;
	client->need_idr = use_avc420;
	client->gfx_uses_progressive = !use_avc420;
	client->bitmap_fallback_active = !use_avc420;
	LeaveCriticalSection(&client->lock);
	client->video_thread = CreateThread(NULL, 0,
	                                    use_avc420 ? video_thread : bitmap_video_thread,
	                                    client, 0, NULL);
	if (!client->video_thread)
	{
		client_stop(client);
		return ERROR_NOT_ENOUGH_MEMORY;
	}
	if (!server_set_stream_requested(client->server, true))
	{
		log_message("ERROR", "RDPGFX NanoKVM agent에 START_STREAM을 보낼 수 없습니다");
		client_stop(client);
		return ERROR_CONNECTION_ABORTED;
	}
	if (use_avc420)
		log_message("INFO", "RDPGFX AVC420 capability 확인 및 surface 초기화 완료");
	else
		log_message("INFO", "RDPGFX Progressive capability 확인 및 surface 초기화 완료");
	return CHANNEL_RC_OK;
}

static UINT on_gfx_frame_ack(RdpgfxServerContext* gfx,
	                         const RDPGFX_FRAME_ACKNOWLEDGE_PDU* acknowledge)
{
	Client* client = (Client*)gfx->custom;
	EnterCriticalSection(&client->lock);
	if (client->inflight && acknowledge->frameId >= client->inflight_frame_id)
		client->inflight = false;
	if (acknowledge->queueDepth == SUSPEND_FRAME_ACKNOWLEDGEMENT)
		client->need_idr = true;
	LeaveCriticalSection(&client->lock);
	return CHANNEL_RC_OK;
}

static bool client_can_send(Client* client)
{
	bool can_send = false;
	EnterCriticalSection(&client->lock);
	can_send = client->gfx_ready && !client->stopping;
	LeaveCriticalSection(&client->lock);
	return can_send;
}

static bool make_idr_payload(Client* client, const uint8_t* data, size_t length, uint8_t** owned,
	                           const uint8_t** payload, size_t* payload_length)
{
	const bool has_sps = h264_contains_nal_type(data, length, 7);
	const bool has_pps = h264_contains_nal_type(data, length, 8);
	const size_t sps_size = has_sps ? 0 : h264_annexb_size(client->sps, client->sps_length);
	const size_t pps_size = has_pps ? 0 : h264_annexb_size(client->pps, client->pps_length);
	if ((sps_size != 0 && !client->sps) || (pps_size != 0 && !client->pps))
		return false;
	if (sps_size == 0 && pps_size == 0)
	{
		*payload = data;
		*payload_length = length;
		return true;
	}
	uint8_t* combined = malloc(sps_size + pps_size + length);
	if (!combined)
		return false;
	size_t offset = 0;
	if (sps_size != 0)
		offset += h264_copy_annexb(combined + offset, client->sps, client->sps_length);
	if (pps_size != 0)
		offset += h264_copy_annexb(combined + offset, client->pps, client->pps_length);
	memcpy(combined + offset, data, length);
	*owned = combined;
	*payload = combined;
	*payload_length = offset + length;
	return true;
}

static bool send_avc420_frame(Client* client, const uint8_t* data, size_t length)
{
	RECTANGLE_16 rect = { .left = 0, .top = 0, .right = client->server->config.width,
		.bottom = client->server->config.height };
	RDPGFX_H264_QUANT_QUALITY quality = { .qpVal = 0, .qualityVal = 100, .qp = 0, .r = 0, .p = 0 };
	RDPGFX_AVC420_BITMAP_STREAM avc = WINPR_C_ARRAY_INIT;
	RDPGFX_SURFACE_COMMAND command = WINPR_C_ARRAY_INIT;
	RDPGFX_START_FRAME_PDU start = WINPR_C_ARRAY_INIT;
	RDPGFX_END_FRAME_PDU end = WINPR_C_ARRAY_INIT;
	UINT error = CHANNEL_RC_OK;

	EnterCriticalSection(&client->lock);
	if (!client->gfx_ready || client->stopping)
	{
		LeaveCriticalSection(&client->lock);
		return true;
	}
	start.frameId = client->next_frame_id++;
	start.timestamp = 0;
	end.frameId = start.frameId;
	LeaveCriticalSection(&client->lock);

	avc.meta.numRegionRects = 1;
	avc.meta.regionRects = &rect;
	avc.meta.quantQualityVals = &quality;
	avc.length = (UINT32)length;
	avc.data = (BYTE*)data;

	command.surfaceId = 1;
	command.codecId = RDPGFX_CODECID_AVC420;
	command.format = PIXEL_FORMAT_BGRX32;
	command.left = 0;
	command.top = 0;
	command.right = client->server->config.width;
	command.bottom = client->server->config.height;
	command.width = client->server->config.width;
	command.height = client->server->config.height;
	command.extra = &avc;

	if (!client->gfx || !client->gfx->SurfaceFrameCommand)
		error = ERROR_INVALID_HANDLE;
	else
		error = client->gfx->SurfaceFrameCommand(client->gfx, &command, &start, &end);
	if (error != CHANNEL_RC_OK)
	{
		EnterCriticalSection(&client->lock);
		client->need_idr = true;
		LeaveCriticalSection(&client->lock);
		log_message("ERROR", "RDPGFX AVC420 frame 전송 실패");
		return false;
	}
	EnterCriticalSection(&client->lock);
	client->bitmap_frames++;
	const uint32_t frame_count = client->bitmap_frames;
	LeaveCriticalSection(&client->lock);
	if (frame_count == 1)
		log_message("INFO", "NanoKVM RTP/H.264 → RDPGFX AVC420 첫 frame 전송 완료");
	return true;
}

static DWORD WINAPI video_thread(LPVOID argument)
{
	Client* client = (Client*)argument;
	RtpClient rtp = { .fd = -1 };
	if (!rtp_client_open(&rtp, client->server->config.video_port))
	{
		log_message("ERROR", "RDPGFX RTP/H.264 receiver를 시작할 수 없습니다");
		client_stop(client);
		return 0;
	}
	log_message("INFO", "RDPGFX RTP/H.264 passthrough 시작");
	uint32_t observed_losses = 0;
	while (!client_should_stop(client))
	{
		uint8_t* data = NULL;
		size_t length = 0;
		if (!rtp_client_read_h264(&rtp, &data, &length))
		{
			const int error = errno;
			if (client_should_stop(client))
				break;
			if (error == EAGAIN || error == EWOULDBLOCK)
				continue;
			log_message("ERROR", "RDPGFX RTP/H.264 frame 수신 실패");
			client_stop(client);
			break;
		}
		if (!data || length == 0)
		{
			free(data);
			continue;
		}
		if (rtp.losses != observed_losses)
		{
			observed_losses = rtp.losses;
			EnterCriticalSection(&client->lock);
			client->need_idr = true;
			LeaveCriticalSection(&client->lock);
			(void)server_send_control(client->server, NANOKVM_CONTROL_IDR_REQUEST, NULL, 0);
			log_message("WARN", "RDPGFX RTP frame loss 감지; NanoKVM agent에 IDR 재동기화를 요청합니다");
		}
		client->last_rtp_received_at = monotonic_milliseconds();
		client->rtp_nals++;
		client->rtp_access_units++;
		if (h264_contains_nal_type(data, length, 7))
			(void)copy_bytes(&client->sps, &client->sps_length, data, length);
		if (h264_contains_nal_type(data, length, 8))
			(void)copy_bytes(&client->pps, &client->pps_length, data, length);
		const bool idr = h264_contains_nal_type(data, length, 5);
		const bool p_frame = h264_contains_nal_type(data, length, 1);
		if (idr)
			client->rtp_idr_units++;
		if (p_frame)
			client->rtp_p_units++;
		bool need_idr = true;
		EnterCriticalSection(&client->lock);
		need_idr = client->need_idr;
		LeaveCriticalSection(&client->lock);
		if (idr)
		{
			EnterCriticalSection(&client->lock);
			client->need_idr = false;
			LeaveCriticalSection(&client->lock);
		}

		if ((idr || (!need_idr && p_frame)) && client_can_send(client))
		{
			uint8_t* owned = NULL;
			const uint8_t* payload = data;
			size_t payload_length = length;
			bool payload_ok = true;
			if (idr)
				payload_ok = make_idr_payload(client, data, length, &owned, &payload, &payload_length);
			if (!payload_ok || !send_avc420_frame(client, payload, payload_length))
				client_stop(client);
			free(owned);
		}
		free(data);
	}
	rtp_client_close(&rtp);
	return 0;
}

static bool bitmap_stream_rfx_supported(const rdpSettings* settings)
{
	const uint32_t supported =
	    freerdp_settings_get_uint32(settings, FreeRDP_SurfaceCommandsSupported);
	return freerdp_settings_get_bool(settings, FreeRDP_RemoteFxCodec) &&
	       freerdp_settings_get_uint32(settings, FreeRDP_RemoteFxCodecId) != 0 &&
	       (supported & SURFCMDS_STREAM_SURFACE_BITS) != 0;
}

static bool bitmap_stream_nsc_supported(const rdpSettings* settings)
{
	const uint32_t supported =
	    freerdp_settings_get_uint32(settings, FreeRDP_SurfaceCommandsSupported);
	return freerdp_settings_get_bool(settings, FreeRDP_NSCodec) &&
	       freerdp_settings_get_uint32(settings, FreeRDP_NSCodecId) != 0 &&
	       (supported & SURFCMDS_SET_SURFACE_BITS) != 0;
}

static bool classic_tile_changed(const uint8_t* previous, const uint8_t* current, uint16_t width,
                                 uint16_t left, uint16_t top, uint16_t columns, uint16_t rows)
{
	if (!previous)
		return true;
	uint32_t changed_pixels = 0;
	for (uint16_t row = 0; row < rows; row++)
	{
		const size_t offset = ((size_t)(top + row) * width + left) * 4U;
		for (uint16_t column = 0; column < columns; column++)
		{
			const size_t pixel = offset + (size_t)column * 4U;
			const uint8_t* before = previous + pixel;
			const uint8_t* after = current + pixel;
			const unsigned blue = before[0] > after[0] ? before[0] - after[0] : after[0] - before[0];
			const unsigned green = before[1] > after[1] ? before[1] - after[1] : after[1] - before[1];
			const unsigned red = before[2] > after[2] ? before[2] - after[2] : after[2] - before[2];
			if (blue >= CLASSIC_PIXEL_DIFF_THRESHOLD ||
			    green >= CLASSIC_PIXEL_DIFF_THRESHOLD || red >= CLASSIC_PIXEL_DIFF_THRESHOLD)
			{
				changed_pixels++;
				if (changed_pixels >= CLASSIC_CHANGED_PIXEL_THRESHOLD)
					return true;
			}
		}
	}
	return false;
}

static void classic_tile_copy(uint8_t* destination, const uint8_t* source, uint16_t width,
	                          uint16_t left, uint16_t top, uint16_t columns, uint16_t rows)
{
	const size_t row_length = (size_t)columns * 4U;
	for (uint16_t row = 0; row < rows; row++)
	{
		const size_t offset = ((size_t)(top + row) * width + left) * 4U;
		memcpy(destination + offset, source + offset, row_length);
	}
}

static bool send_classic_bitmap_frame(Client* client, const uint8_t* bgra, size_t length)
{
	const uint16_t width = client->render_width;
	const uint16_t height = client->render_height;
	const size_t expected_length = (size_t)width * height * 4U;
	const rdpSettings* settings = client->context.settings;
	const uint16_t bits_per_pixel = 16;
	BITMAP_DATA rectangles[CLASSIC_BITMAP_BATCH] = WINPR_C_ARRAY_INIT;
	BITMAP_UPDATE bitmap = WINPR_C_ARRAY_INIT;
	const uint32_t negotiated_update_size =
	    freerdp_settings_get_uint32(settings, FreeRDP_MultifragMaxRequestSize);
	const uint32_t max_update_size =
	    negotiated_update_size < CLASSIC_MAX_UPDATE_SIZE ? negotiated_update_size : CLASSIC_MAX_UPDATE_SIZE;
	uint32_t update_size = 1024U;
	uint16_t rectangle_count = 0;
	uint32_t frame_tiles = 0;
	uint64_t frame_bytes = 0;

	if (!client->context.update || !client->context.update->BitmapUpdate ||
	    settings == NULL || !client->interleaved ||
	    !client->classic_encoded ||
	    length != expected_length ||
	    client_should_stop(client))
		return false;
	if (!client->previous_bitmap)
	{
		client->previous_bitmap = malloc(expected_length);
		client->previous_bitmap_length = client->previous_bitmap ? expected_length : 0;
	}
	if (!client->previous_bitmap || client->previous_bitmap_length != expected_length)
		return false;

	for (uint16_t top = 0; top < height; top += CLASSIC_TILE_HEIGHT)
	{
		const uint16_t rows = MIN(CLASSIC_TILE_HEIGHT, (uint16_t)(height - top));
		for (uint16_t left = 0; left < width; left += CLASSIC_TILE_WIDTH)
		{
			const uint16_t columns = MIN(CLASSIC_TILE_WIDTH, (uint16_t)(width - left));
			if (client->previous_bitmap_valid &&
			    !classic_tile_changed(client->previous_bitmap, bgra, width, left, top, columns, rows))
				continue;
			BITMAP_DATA* rectangle = &rectangles[rectangle_count];
			uint32_t encoded_length = CLASSIC_TILE_MAX_ENCODED;
			uint8_t* encoded_data =
			    client->classic_encoded + (size_t)rectangle_count * CLASSIC_TILE_MAX_ENCODED;
			if ((columns % 4) != 0 ||
			    !interleaved_compress(client->interleaved, encoded_data,
			                          &encoded_length, columns, rows, bgra,
			                          PIXEL_FORMAT_BGRX32, (uint32_t)width * 4U,
			                          left, top, NULL, bits_per_pixel))
			{
				encoded_data = NULL;
			}
			if (!encoded_data || encoded_length == 0 || encoded_length > CLASSIC_TILE_MAX_ENCODED)
				return false;
			if (rectangle_count > 0 &&
			    update_size + encoded_length + 16U >= max_update_size)
			{
				bitmap.number = rectangle_count;
				bitmap.rectangles = rectangles;
				bitmap.skipCompression = FALSE;
				if (!client->context.update->BitmapUpdate(&client->context, &bitmap))
					return false;
				memcpy(client->classic_encoded, encoded_data, encoded_length);
				encoded_data = client->classic_encoded;
				rectangle = &rectangles[0];
				rectangle_count = 0;
				update_size = 1024U;
			}
			rectangle->destLeft = left;
			rectangle->destTop = top;
			rectangle->destRight = left + columns - 1;
			rectangle->destBottom = top + rows - 1;
			rectangle->width = columns;
			rectangle->height = rows;
			rectangle->bitsPerPixel = bits_per_pixel;
			rectangle->bitmapLength = WINPR_ASSERTING_INT_CAST(uint16_t, encoded_length);
			rectangle->bitmapDataStream = encoded_data;
			rectangle->compressed = TRUE;
			rectangle->cbCompFirstRowSize = 0;
			rectangle->cbCompMainBodySize = encoded_length;
			rectangle->cbScanWidth = columns * (bits_per_pixel / 8U);
			rectangle->cbUncompressedSize = columns * rows * (bits_per_pixel / 8U);
			rectangle_count++;
			update_size += encoded_length + 16U;
			classic_tile_copy(client->previous_bitmap, bgra, width, left, top, columns, rows);
			frame_tiles++;
			frame_bytes += encoded_length;
			if (rectangle_count == CLASSIC_BITMAP_BATCH)
			{
				bitmap.number = rectangle_count;
				bitmap.rectangles = rectangles;
				bitmap.skipCompression = FALSE;
				if (!client->context.update->BitmapUpdate(&client->context, &bitmap))
					return false;
				rectangle_count = 0;
				update_size = 1024U;
			}
		}
	}
	if (rectangle_count > 0)
	{
		bitmap.number = rectangle_count;
		bitmap.rectangles = rectangles;
		bitmap.skipCompression = FALSE;
		if (!client->context.update->BitmapUpdate(&client->context, &bitmap))
			return false;
	}
	EnterCriticalSection(&client->lock);
	client->classic_tiles_sent += frame_tiles;
	client->classic_bytes_sent += frame_bytes;
	LeaveCriticalSection(&client->lock);
	client->previous_bitmap_valid = true;
	return true;
}

static bool send_progressive_frame(Client* client, const uint8_t* bgra, size_t length)
{
	const uint16_t width = client->render_width;
	const uint16_t height = client->render_height;
	const size_t expected_length = (size_t)width * height * 4U;
	REGION16 region = WINPR_C_ARRAY_INIT;
	RECTANGLE_16 rect = { .left = 0, .top = 0, .right = width, .bottom = height };
	RDPGFX_SURFACE_COMMAND command = WINPR_C_ARRAY_INIT;
	RDPGFX_START_FRAME_PDU start = WINPR_C_ARRAY_INIT;
	RDPGFX_END_FRAME_PDU end = WINPR_C_ARRAY_INIT;

	if (!client->progressive || !client->gfx || !client->gfx->SurfaceFrameCommand ||
	    length != expected_length || client_should_stop(client))
		return false;
	region16_init(&region);
	if (!region16_union_rect(&region, &region, &rect))
	{
		region16_uninit(&region);
		return false;
	}
	const int encoded = progressive_compress(
	    client->progressive, bgra, WINPR_ASSERTING_INT_CAST(uint32_t, length),
	    PIXEL_FORMAT_BGRX32, width, height, (uint32_t)width * 4U, &region,
	    &command.data, &command.length);
	region16_uninit(&region);
	if (encoded < 0)
		return false;
	if (encoded == 0)
		return true;

	EnterCriticalSection(&client->lock);
	start.frameId = client->next_frame_id++;
	start.timestamp = (UINT32)monotonic_milliseconds();
	end.frameId = start.frameId;
	LeaveCriticalSection(&client->lock);
	command.surfaceId = 1;
	command.codecId = RDPGFX_CODECID_CAPROGRESSIVE;
	command.format = PIXEL_FORMAT_BGRX32;
	command.left = 0;
	command.top = 0;
	command.right = width;
	command.bottom = height;
	command.width = width;
	command.height = height;
	const UINT error = client->gfx->SurfaceFrameCommand(client->gfx, &command, &start, &end);
	command.data = NULL;
	if (error != CHANNEL_RC_OK)
	{
		log_message("ERROR", "RDPGFX Progressive frame 전송 실패");
		return false;
	}
	return true;
}

static bool send_bitmap_frame(Client* client, const uint8_t* bgra, size_t length)
{
	const uint16_t width = client->render_width;
	const uint16_t height = client->render_height;
	const size_t expected_length = (size_t)width * height * 4U;
	rdpSettings* settings = client->context.settings;
	rdpUpdate* update = client->context.update;
	SURFACE_BITS_COMMAND command = WINPR_C_ARRAY_INIT;
	RFX_RECT rect = { .x = 0, .y = 0, .width = width, .height = height };

	if (!settings || !update || length != expected_length || client_should_stop(client))
		return false;
	if (client->gfx_uses_progressive)
	{
		if (!send_progressive_frame(client, bgra, length))
			return false;
		goto sent;
	}
	if (!client->bitmap_uses_rfx && !client->nsc)
		goto sent_classic;
	if (!update->SurfaceBits || !client->bitmap_stream)
		return false;
	Stream_Clear(client->bitmap_stream);
	Stream_SetPosition(client->bitmap_stream, 0);
	if (client->bitmap_uses_rfx)
	{
		if (!client->rfx || !bitmap_stream_rfx_supported(settings))
			return false;
		rfx_context_set_pixel_format(client->rfx, PIXEL_FORMAT_BGRX32);
		if (!rfx_compose_message(client->rfx, client->bitmap_stream, &rect, 1, bgra, width, height,
		                         (uint32_t)width * 4U))
			return false;
		command.cmdType = CMDTYPE_STREAM_SURFACE_BITS;
		command.bmp.codecID =
		    WINPR_ASSERTING_INT_CAST(uint16_t,
		                             freerdp_settings_get_uint32(settings, FreeRDP_RemoteFxCodecId));
	}
	else
	{
		if (!client->nsc || !bitmap_stream_nsc_supported(settings))
			return false;
		if (!nsc_context_set_parameters(client->nsc, NSC_COLOR_FORMAT, PIXEL_FORMAT_BGRX32) ||
		    !nsc_compose_message(client->nsc, client->bitmap_stream, bgra, width, height,
		                         (uint32_t)width * 4U))
			return false;
		command.cmdType = CMDTYPE_SET_SURFACE_BITS;
		command.bmp.codecID =
		    WINPR_ASSERTING_INT_CAST(uint16_t,
		                             freerdp_settings_get_uint32(settings, FreeRDP_NSCodecId));
	}
	command.destLeft = 0;
	command.destTop = 0;
	command.destRight = width;
	command.destBottom = height;
	command.bmp.bpp = 32;
	command.bmp.flags = 0;
	command.bmp.width = width;
	command.bmp.height = height;
	command.bmp.bitmapDataLength =
	    WINPR_ASSERTING_INT_CAST(uint32_t, Stream_GetPosition(client->bitmap_stream));
	command.bmp.bitmapData = Stream_Buffer(client->bitmap_stream);
	if (!update->SurfaceBits(&client->context, &command))
		return false;

sent_classic:
	if (!client->bitmap_uses_rfx && !client->nsc && !send_classic_bitmap_frame(client, bgra, length))
		return false;
sent:
	client->bitmap_frames++;
	if (client->bitmap_frames == 1)
	{
		if (client->gfx_uses_progressive)
			log_message("INFO", "FoldVNC H.264 → FFmpeg BGRA → RDPGFX Progressive 첫 frame 전송 완료");
		else
			log_message("INFO", "FoldVNC H.264 → FFmpeg BGRA → RDP bitmap 첫 frame 전송 완료");
	}
	return true;
}

static bool on_decoded_bitmap_frame(void* context, const uint8_t* bgra, size_t length)
{
	Client* client = context;
	const uint64_t now = monotonic_milliseconds();
	const size_t expected_length = (size_t)client->render_width * client->render_height * 4U;
	uint8_t* bitmap = NULL;
	size_t bitmap_length = 0;
	uint8_t slot = 0;
	if (length != expected_length || client_should_stop(client))
		return false;
	client->decoded_frames++;
	EnterCriticalSection(&client->lock);
	if (client->bitmap_queue_count == BITMAP_QUEUE_CAPACITY)
	{
		slot = client->bitmap_queue_head;
		bitmap = client->bitmap_queue[slot];
		bitmap_length = client->bitmap_queue_lengths[slot];
		client->bitmap_queue_head =
		    (uint8_t)((client->bitmap_queue_head + 1U) % BITMAP_QUEUE_CAPACITY);
		client->bitmap_queue_count--;
		client->bitmap_queue_drops++;
	}
	else if (client->pending_bitmap)
	{
		bitmap = client->pending_bitmap;
		bitmap_length = client->pending_bitmap_length;
		client->pending_bitmap = NULL;
		client->pending_bitmap_length = 0;
	}
	if (!bitmap)
	{
		bitmap = malloc(expected_length);
		bitmap_length = bitmap ? expected_length : 0;
	}
	if (!bitmap || bitmap_length != expected_length)
	{
		free(bitmap);
		LeaveCriticalSection(&client->lock);
		return false;
	}
	slot = (uint8_t)((client->bitmap_queue_head + client->bitmap_queue_count) %
	                 BITMAP_QUEUE_CAPACITY);
	memcpy(bitmap, bgra, expected_length);
	client->bitmap_queue[slot] = bitmap;
	client->bitmap_queue_lengths[slot] = expected_length;
	client->bitmap_queue_queued_at[slot] = now;
	client->bitmap_queue_count++;
	client->bitmap_pending = true;
	client->bitmap_queued_frames++;
	client->last_decode_latency_ms = now - client->last_rtp_received_at;
	LeaveCriticalSection(&client->lock);
	if (client->bitmap_ready_event)
		(void)SetEvent(client->bitmap_ready_event);
	return true;
}

static bool client_flush_pending_bitmap(Client* client)
{
	uint8_t* bitmap = NULL;
	uint8_t* stale[BITMAP_QUEUE_CAPACITY] = { 0 };
	size_t stale_count = 0;
	size_t bitmap_length = 0;
	bool sent = true;
	const uint64_t now = monotonic_milliseconds();
	if (client->bitmap_ready_event)
		(void)ResetEvent(client->bitmap_ready_event);
	if (client->peer && client->peer->IsWriteBlocked && client->peer->DrainOutputBuffer &&
	    client->peer->IsWriteBlocked(client->peer))
	{
		(void)client->peer->DrainOutputBuffer(client->peer);
		if (client->peer->IsWriteBlocked(client->peer))
			return true;
	}
	EnterCriticalSection(&client->lock);
	client->bitmap_flushes++;
	while (client->bitmap_queue_count > 0)
	{
		const uint8_t stale_slot = client->bitmap_queue_head;
		if (now - client->bitmap_queue_queued_at[stale_slot] <= BITMAP_MAX_QUEUE_AGE_MS)
			break;
		stale[stale_count++] = client->bitmap_queue[stale_slot];
		client->bitmap_queue[stale_slot] = NULL;
		client->bitmap_queue_lengths[stale_slot] = 0;
		client->bitmap_queue_queued_at[stale_slot] = 0;
		client->bitmap_queue_head =
		    (uint8_t)((client->bitmap_queue_head + 1U) % BITMAP_QUEUE_CAPACITY);
		client->bitmap_queue_count--;
		client->bitmap_queue_drops++;
		client->bitmap_stale_drops++;
	}
	if (client->bitmap_queue_count == 0)
	{
		client->bitmap_empty_flushes++;
		LeaveCriticalSection(&client->lock);
		for (size_t index = 0; index < stale_count; index++)
			free(stale[index]);
		return true;
	}
	if (client->bitmap_last_send_started_at != 0 &&
	    now - client->bitmap_last_send_started_at < BITMAP_FRAME_INTERVAL_MS)
	{
		LeaveCriticalSection(&client->lock);
		for (size_t index = 0; index < stale_count; index++)
			free(stale[index]);
		return true;
	}
	const uint8_t newest_slot = (uint8_t)((client->bitmap_queue_head +
	                                      client->bitmap_queue_count - 1U) %
	                                     BITMAP_QUEUE_CAPACITY);
	bitmap = client->bitmap_queue[newest_slot];
	bitmap_length = client->bitmap_queue_lengths[newest_slot];
	for (uint8_t index = 0; index < client->bitmap_queue_count; index++)
	{
		const uint8_t slot =
		    (uint8_t)((client->bitmap_queue_head + index) % BITMAP_QUEUE_CAPACITY);
		if (slot == newest_slot)
			continue;
		stale[stale_count++] = client->bitmap_queue[slot];
		client->bitmap_queue[slot] = NULL;
		client->bitmap_queue_lengths[slot] = 0;
		client->bitmap_queue_queued_at[slot] = 0;
		client->bitmap_queue_drops++;
		client->bitmap_stale_drops++;
	}
	client->bitmap_queue[newest_slot] = NULL;
	client->bitmap_queue_lengths[newest_slot] = 0;
	client->bitmap_queue_queued_at[newest_slot] = 0;
	client->bitmap_queue_head = (uint8_t)((newest_slot + 1U) % BITMAP_QUEUE_CAPACITY);
	client->bitmap_queue_count = 0;
	client->bitmap_pending = false;
	client->bitmap_last_send_started_at = now;
	LeaveCriticalSection(&client->lock);
	for (size_t index = 0; index < stale_count; index++)
		free(stale[index]);
	if (!bitmap)
	{
		log_message("ERROR", "RDP bitmap queue가 비어 있지 않은데 frame buffer가 없습니다");
		client_stop(client);
		return false;
	}

	const uint64_t started_at = monotonic_milliseconds();
	sent = send_bitmap_frame(client, bitmap, bitmap_length);
	const uint64_t completed_at = monotonic_milliseconds();
	EnterCriticalSection(&client->lock);
	if (sent)
	{
		client->last_rdp_send_ms = completed_at - started_at;
	}
	if (!client->pending_bitmap)
	{
		client->pending_bitmap = bitmap;
		client->pending_bitmap_length = bitmap_length;
		bitmap = NULL;
	}
	LeaveCriticalSection(&client->lock);
	free(bitmap);
	if (!sent)
	{
		log_message("ERROR", "RDP bitmap frame 전송 실패");
		client_stop(client);
	}
	return sent;
}

static DWORD WINAPI bitmap_video_thread(LPVOID argument)
{
	Client* client = (Client*)argument;
	RtpClient rtp = { .fd = -1 };
	FfmpegDecoder decoder = { .pid = -1, .input = -1, .output = -1 };
	const uint16_t width = client->render_width;
	const uint16_t height = client->render_height;
	bool backend_ready = false;

	for (unsigned attempt = 0; attempt < 10 && !client_should_stop(client); attempt++)
	{
		if (rtp_client_open(&rtp, client->server->config.video_port) &&
		    ffmpeg_decoder_start(&decoder, width, height, on_decoded_bitmap_frame, client))
		{
			backend_ready = true;
			break;
		}
		ffmpeg_decoder_stop(&decoder);
		rtp_client_close(&rtp);
		Sleep(1000);
	}
	if (!backend_ready)
	{
		log_message("ERROR", "RTP/H.264 receiver 또는 FFmpeg decoder를 시작할 수 없습니다");
		client_stop(client);
		goto out;
	}
	log_message("INFO", "RTP/H.264 receiver와 FFmpeg BGRA decoder 시작 완료");
	uint32_t observed_losses = 0;
	while (!client_should_stop(client))
	{
		uint8_t* data = NULL;
		size_t length = 0;
		if (!rtp_client_read_h264(&rtp, &data, &length))
		{
			const int error = errno;
			if (client_should_stop(client))
				break;
			if (error == EAGAIN || error == EWOULDBLOCK)
				continue;
			log_message("ERROR", "RTP/H.264 frame 수신 실패");
			client_stop(client);
			break;
		}
		if (!data || length == 0)
		{
			free(data);
			continue;
		}
		client->last_rtp_received_at = monotonic_milliseconds();
		client->rtp_nals++;
		client->rtp_access_units++;
		if (h264_contains_nal_type(data, length, 5))
			client->rtp_idr_units++;
		if (h264_contains_nal_type(data, length, 1))
			client->rtp_p_units++;
		const size_t annexb_length = h264_annexb_size(data, length);
		uint8_t* annexb = malloc(annexb_length);
		if (!annexb)
		{
			free(data);
			client_stop(client);
			break;
		}
		(void)h264_copy_annexb(annexb, data, length);
		const bool pushed = ffmpeg_decoder_push(&decoder, annexb, annexb_length);
		free(annexb);
		free(data);
		if (!pushed)
		{
			log_message("ERROR", "FFmpeg H.264 decoder 입력 실패");
			client_stop(client);
			break;
		}
		if (rtp.losses != observed_losses)
		{
			observed_losses = rtp.losses;
			(void)server_send_control(client->server, NANOKVM_CONTROL_IDR_REQUEST, NULL, 0);
			log_message("WARN", "RTP frame loss 감지; NanoKVM agent에 IDR 재동기화를 요청합니다");
		}
	}

out:
	ffmpeg_decoder_stop(&decoder);
	rtp_client_close(&rtp);
	return 0;
}

static BOOL on_keyboard(rdpInput* input, UINT16 flags, UINT8 code)
{
	Client* client = (Client*)input->context;
	uint8_t mapped_code = code;
	bool mapped_extended = (flags & KBD_FLAGS_EXTENDED) != 0;
	hid_map_scancode(code, mapped_extended, client->server->config.swap_alt_command,
	                 client->server->config.right_alt_as_capslock,
	                 &mapped_code, &mapped_extended);
	const uint8_t payload[3] = { mapped_code, mapped_extended,
		(flags & KBD_FLAGS_RELEASE) != 0 };
	const bool sent = server_send_control(client->server, NANOKVM_CONTROL_KEY, payload, sizeof(payload));
	if (sent && !client->keyboard_input_logged)
	{
		log_message("INFO", "RDP keyboard scancode → NanoKVM agent HID 전달 확인");
		client->keyboard_input_logged = true;
	}
	return sent;
}

static BOOL on_unicode_keyboard(rdpInput* input, UINT16 flags, UINT16 code)
{
	Client* client = (Client*)input->context;
	if ((flags & KBD_FLAGS_RELEASE) != 0)
		return TRUE;
	if (code == 0 || (code >= 0xd800U && code <= 0xdfffU))
	{
		char message[128];
		(void)snprintf(message, sizeof(message),
		               "지원하지 않는 RDP Unicode keyboard code unit U+%04X", code);
		log_message("WARN", message);
		return FALSE;
	}

	uint8_t payload[3] = { 0 };
	uint16_t length = 0;
	if (code <= 0x007fU)
	{
		payload[0] = (uint8_t)code;
		length = 1;
	}
	else if (code <= 0x07ffU)
	{
		payload[0] = (uint8_t)(0xc0U | (code >> 6U));
		payload[1] = (uint8_t)(0x80U | (code & 0x3fU));
		length = 2;
	}
	else
	{
		payload[0] = (uint8_t)(0xe0U | (code >> 12U));
		payload[1] = (uint8_t)(0x80U | ((code >> 6U) & 0x3fU));
		payload[2] = (uint8_t)(0x80U | (code & 0x3fU));
		length = 3;
	}

	const bool sent = server_send_control(client->server, NANOKVM_CONTROL_TEXT_UTF8, payload, length);
	if (!sent)
		log_message("WARN", "RDP Unicode keyboard text를 NanoKVM agent에 전달하지 못했습니다");
	return sent;
}

static bool client_force_source_desktop_size(Client* client)
{
	rdpSettings* settings = client->context.settings;
	rdpUpdate* update = client->context.update;
	const uint32_t width = client->server->config.width;
	const uint32_t height = client->server->config.height;
	if (!settings || !update || !update->DesktopResize)
		return false;
	if (freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth) == width &&
	    freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight) == height)
		return true;
	if (!freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, width) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, height) ||
	    !update->DesktopResize(update->context))
		return false;
	log_message("INFO", "RDP desktop을 NanoKVM 원본 1920x1080으로 재협상");
	return true;
}

static bool client_set_render_size(Client* client)
{
	const rdpSettings* settings = client->context.settings;
	if (!settings)
		return false;
	const uint32_t width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
	const uint32_t height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
	if (width == 0 || height == 0 || width > UINT16_MAX || height > UINT16_MAX)
		return false;
	client->render_width = (uint16_t)width;
	client->render_height = (uint16_t)height;
	char message[128];
	(void)snprintf(message, sizeof(message),
	               "RDP 협상 desktop %ux%u에 맞춰 gateway bitmap을 확대합니다", width, height);
	log_message("INFO", message);
	return true;
}

static BOOL client_release_all_inputs(Client* client, const char* reason)
{
	client->relative_buttons = 0;
	const bool sent = server_send_control(client->server, NANOKVM_CONTROL_RELEASE_ALL, NULL, 0);
	if (sent && reason)
	{
		char message[128];
		(void)snprintf(message, sizeof(message),
		               "RDP input state resync: %s → RELEASE_ALL 전송", reason);
		log_message("INFO", message);
	}
	return sent;
}

static BOOL on_synchronize(rdpInput* input, UINT32 flags)
{
	(void)flags;
	Client* client = (Client*)input->context;
	return client_release_all_inputs(client, "SynchronizeEvent");
}

static BOOL on_focus_in(rdpInput* input, UINT16 toggle_states)
{
	(void)toggle_states;
	Client* client = (Client*)input->context;
	return client_release_all_inputs(client, "FocusInEvent");
}

static BOOL on_mouse(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
{
	Client* client = (Client*)input->context;
	const uint16_t width = client->server->config.width;
	const uint16_t height = client->server->config.height;
	uint8_t payload[12] = { 0 };
	protocol_write_u16(payload, hid_clamp_absolute(x, width));
	protocol_write_u16(payload + 2, hid_clamp_absolute(y, height));
	protocol_write_u16(payload + 4, width);
	protocol_write_u16(payload + 6, height);
	protocol_write_u16(payload + 8, flags);
	/* wheel 전용 이벤트(x=y=0)에서는 POINTER_ABS(0,0) touch report를 보내지 않는다.
	 * touch report가 HID gadget/USB를 점유해 wheel report가 밀리는 간섭을 방지. */
	const bool has_wheel = (flags & 0x0600U) != 0;
	const bool position_ok = has_wheel ||
	                         server_send_control(client->server, NANOKVM_CONTROL_POINTER_ABS,
	                                              payload, sizeof(payload));
	const bool wheel_ok = !has_wheel ||
	                      server_send_control(client->server, NANOKVM_CONTROL_WHEEL, payload + 8, 2);
	if (position_ok && !client->pointer_input_logged)
	{
		log_message("INFO", "RDP absolute pointer → NanoKVM agent HID 전달 확인");
		client->pointer_input_logged = true;
	}
	if (wheel_ok && (flags & PTR_FLAGS_WHEEL) != 0 &&
	    !client->wheel_input_logged)
	{
		log_message("INFO", "RDP wheel → NanoKVM agent HID 전달 확인");
		client->wheel_input_logged = true;
	}
	return position_ok && wheel_ok;
}

static BOOL on_extended_mouse(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
{
	return on_mouse(input, flags, x, y);
}

static BOOL on_relative_mouse(rdpInput* input, UINT16 flags, INT16 x_delta, INT16 y_delta)
{
	Client* client = (Client*)input->context;
	if ((flags & PTR_FLAGS_BUTTON1) != 0)
	{
		if ((flags & PTR_FLAGS_DOWN) != 0)
			client->relative_buttons |= 0x01;
		else
			client->relative_buttons &= (uint8_t)~0x01U;
	}
	if ((flags & PTR_FLAGS_BUTTON2) != 0)
	{
		if ((flags & PTR_FLAGS_DOWN) != 0)
			client->relative_buttons |= 0x02;
		else
			client->relative_buttons &= (uint8_t)~0x02U;
	}
	if ((flags & PTR_FLAGS_BUTTON3) != 0)
	{
		if ((flags & PTR_FLAGS_DOWN) != 0)
			client->relative_buttons |= 0x04;
		else
			client->relative_buttons &= (uint8_t)~0x04U;
	}
	uint8_t payload[5] = { 0 };
	protocol_write_u16(payload, (uint16_t)x_delta);
	protocol_write_u16(payload + 2, (uint16_t)y_delta);
	payload[4] = client->relative_buttons;
	const bool position_ok = server_send_control(client->server, NANOKVM_CONTROL_POINTER_REL,
	                                              payload, sizeof(payload));
	uint8_t wheel_payload[2] = { 0 };
	protocol_write_u16(wheel_payload, flags);
	const bool wheel_ok = (flags & PTR_FLAGS_WHEEL) == 0 ||
	                      server_send_control(client->server, NANOKVM_CONTROL_WHEEL,
	                                           wheel_payload, sizeof(wheel_payload));
	if (wheel_ok && (flags & PTR_FLAGS_WHEEL) != 0 &&
	    !client->wheel_input_logged)
	{
		log_message("INFO", "RDP relative wheel → NanoKVM agent HID 전달 확인");
		client->wheel_input_logged = true;
	}
	return position_ok && wheel_ok;
}

static BOOL client_context_new(freerdp_peer* peer, rdpContext* context)
{
	Client* client = (Client*)context;
	Server* server = (Server*)peer->ContextExtra;
	if (!server || !InitializeCriticalSectionAndSpinCount(&client->lock, 4000))
		return FALSE;
	client->server = server;
	client->peer = peer;
	client->need_idr = true;
	hid_init(&client->hid, server->config.keyboard, server->config.mouse, server->config.touch);
	if (server->config.direct_gfx)
	{
		client->vcm = WTSOpenServerA((LPSTR)context);
		if (!client->vcm || client->vcm == INVALID_HANDLE_VALUE)
			goto fail;
	}

	return TRUE;

fail:
	return FALSE;
}

static bool client_claim_active(Client* client)
{
	Server* server = client->server;
	EnterCriticalSection(&server->lock);
	if (server->active)
	{
		LeaveCriticalSection(&server->lock);
		log_message("WARN", "single-client 제한으로 새 RDP 연결을 거부합니다");
		return false;
	}
	server->active = client;
	client->owns_active_client = true;
	LeaveCriticalSection(&server->lock);
	return true;
}

static void client_context_free(freerdp_peer* peer, rdpContext* context)
{
	(void)peer;
	Client* client = (Client*)context;
	client_stop(client);
	if (client->owns_active_client)
	{
		(void)client_release_all_inputs(client, NULL);
		(void)server_set_stream_requested(client->server, false);
	}
	if (client->video_thread)
	{
		(void)WaitForSingleObject(client->video_thread, 3000);
		(void)CloseHandle(client->video_thread);
	}
	if (client->bitmap_ready_event)
		(void)CloseHandle(client->bitmap_ready_event);
	if (client->gfx)
		rdpgfx_server_context_free(client->gfx);
	if (client->rfx)
		rfx_context_free(client->rfx);
	if (client->nsc)
		nsc_context_free(client->nsc);
	if (client->progressive)
		progressive_context_free(client->progressive);
	if (client->interleaved)
		bitmap_interleaved_context_free(client->interleaved);
	if (client->bitmap_stream)
		Stream_Free(client->bitmap_stream, TRUE);
	free(client->pending_bitmap);
	for (size_t index = 0; index < BITMAP_QUEUE_CAPACITY; index++)
		free(client->bitmap_queue[index]);
	free(client->previous_bitmap);
	free(client->classic_encoded);
	if (client->vcm && client->vcm != INVALID_HANDLE_VALUE)
		WTSCloseServer(client->vcm);
	hid_release_all(&client->hid);
	free(client->sps);
	free(client->pps);
	if (client->server && client->owns_active_client)
	{
		EnterCriticalSection(&client->server->lock);
		if (client->server->active == client)
			client->server->active = NULL;
		LeaveCriticalSection(&client->server->lock);
	}
	DeleteCriticalSection(&client->lock);
	log_message("INFO", "RDP client 연결 종료 및 USB HID release 완료");
}

static bool client_prepare_gfx(Client* client)
{
	if (client->gfx)
		return true;
	client->gfx = rdpgfx_server_context_new(client->vcm);
	if (!client->gfx)
		return false;
	client->gfx->rdpcontext = &client->context;
	client->gfx->custom = client;
	client->gfx->CapsAdvertise = on_gfx_caps_advertise;
	client->gfx->FrameAcknowledge = on_gfx_frame_ack;
	if (!client->gfx->Initialize || !client->gfx->Initialize(client->gfx, TRUE))
		return false;
	client->direct_gfx_active = true;
	return true;
}

static bool client_prepare_bitmap(Client* client)
{
	const rdpSettings* settings = client->context.settings;
	if (!settings)
		return false;
	if (!client->bitmap_ready_event)
	{
		client->bitmap_ready_event = CreateEvent(NULL, FALSE, FALSE, NULL);
		if (!client->bitmap_ready_event)
			return false;
	}
	client->bitmap_fallback_active = true;
	client->bitmap_uses_rfx = bitmap_stream_rfx_supported(settings);
	if (client->bitmap_uses_rfx)
	{
		client->rfx = rfx_context_new_ex(
		    TRUE, freerdp_settings_get_uint32(settings, FreeRDP_ThreadingFlags));
		if (!client->rfx || !rfx_context_reset(client->rfx, client->render_width,
		                                      client->render_height))
			return false;
	}
	else if (bitmap_stream_nsc_supported(settings))
	{
		client->nsc = nsc_context_new();
		if (!client->nsc)
			return false;
	}
	else
	{
		const uint32_t color_depth = freerdp_settings_get_uint32(settings, FreeRDP_ColorDepth);
		client->classic_encoded = calloc(CLASSIC_BITMAP_BATCH, CLASSIC_TILE_MAX_ENCODED);
		if (!client->classic_encoded)
			return false;
		if (color_depth != 16 && color_depth != 24 && color_depth != 32)
		{
			log_message("ERROR", "client가 지원하지 않는 classic bitmap 색 깊이를 요청했습니다");
			return false;
		}
		if (color_depth == 24 &&
		    !freerdp_settings_set_uint32((rdpSettings*)settings, FreeRDP_ColorDepth, 16))
			return false;
		client->interleaved = bitmap_interleaved_context_new(TRUE);
		if (!client->interleaved)
			return false;
		log_message("INFO", "RemoteFX/NSCodec 없이 16-bit interleaved BitmapUpdate 경로를 사용합니다");
	}
	if (client->bitmap_uses_rfx || client->nsc)
	{
		client->bitmap_stream = Stream_New(NULL, 65536);
		if (!client->bitmap_stream)
			return false;
	}
	client->video_thread = CreateThread(NULL, 0, bitmap_video_thread, client, 0, NULL);
	if (!client->video_thread)
		return false;
	if (!server_set_stream_requested(client->server, true))
	{
		log_message("ERROR", "NanoKVM agent에 START_STREAM을 보낼 수 없습니다");
		return false;
	}
	return true;
}

static BOOL peer_post_connect(freerdp_peer* peer)
{
	Client* client = (Client*)peer->context;
	if (!client_force_source_desktop_size(client))
		return FALSE;
	if (!client_set_render_size(client))
		return FALSE;
	if (!client_claim_active(client))
		return FALSE;
	if (client->server->config.direct_gfx)
	{
		if (!client_prepare_gfx(client))
			return FALSE;
		client->gfx_wait_started_at = monotonic_milliseconds();
		log_message("INFO", "RDP session activation 완료; RDPGFX dynamic channel open 대기 중");
	}
	else
	{
		if (!client_prepare_bitmap(client))
			return FALSE;
		log_message("INFO", "RDP session activation 완료; RTP/H.264 bitmap backend 시작");
	}
	return TRUE;
}

static bool client_process_dynamic_channels(Client* client)
{
	if (client->gfx_wait_started_at == 0 ||
	    !WTSVirtualChannelManagerIsChannelJoined(client->vcm, DRDYNVC_SVC_CHANNEL_NAME))
		return true;

	/* This only flushes the VCM's local queue; it does not read the peer transport. */
	if (!WTSVirtualChannelManagerCheckFileDescriptor(client->vcm))
		return false;

	if (!client->gfx)
		return true;
	if (!client->gfx_opened &&
	    WTSVirtualChannelManagerGetDrdynvcState(client->vcm) == DRDYNVC_STATE_READY)
	{
		if (!client->gfx->Open || !client->gfx->Open(client->gfx))
			return false;
		client->gfx_opened = true;
		client->gfx_opened_at = monotonic_milliseconds();
		log_message("INFO", "RDPGFX dynamic channel open 완료; client capability 대기 중");
	}
	if (client->gfx_opened)
	{
		HANDLE event = rdpgfx_server_get_event_handle(client->gfx);
		if (event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0)
		{
			const UINT error = rdpgfx_server_handle_messages(client->gfx);
			if (error != CHANNEL_RC_OK)
			{
				log_message("ERROR", "RDPGFX channel message 처리 실패");
				return false;
			}
		}
	}
	return true;
}

static bool client_check_gfx_timeout(Client* client)
{
	if (!client->direct_gfx_active || client->gfx_wait_started_at == 0 || client->gfx_ready)
		return true;
	const uint64_t started_at = client->gfx_opened ? client->gfx_opened_at : client->gfx_wait_started_at;
	if (monotonic_milliseconds() - started_at <= 5000)
		return true;
	client->direct_gfx_active = false;
	client->bitmap_fallback_active = true;
	client->gfx_wait_started_at = 0;
	if (!client_prepare_bitmap(client))
	{
		log_message("ERROR", "RDPGFX 미지원 client의 classic bitmap fallback을 시작할 수 없습니다");
		return false;
	}
	if (client->gfx_opened)
		log_message("INFO", "RDPGFX capability 응답이 없는 client를 classic bitmap backend로 전환합니다");
	else
		log_message("INFO", "RDPGFX dynamic channel이 없는 client를 classic bitmap backend로 전환합니다");
	return true;
}

static bool configure_peer(freerdp_peer* peer, Server* server)
{
	peer->ContextSize = sizeof(Client);
	peer->ContextNew = client_context_new;
	peer->ContextFree = client_context_free;
	if (!freerdp_peer_context_new(peer))
		return false;

	rdpSettings* settings = peer->context->settings;
	rdpPrivateKey* private_key = freerdp_key_new_from_file(server->config.private_key);
	rdpCertificate* certificate = freerdp_certificate_new_from_file(server->config.certificate);
	if (!private_key || !certificate)
		return false;
	if (!freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, private_key, 1) ||
	    !freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, certificate, 1) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline,
	                               server->config.direct_gfx) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_GfxH264, server->config.direct_gfx) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_GfxProgressive,
	                               server->config.direct_gfx) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_GfxProgressiveV2,
	                               server->config.direct_gfx) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, TRUE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_NSCodec, TRUE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_FrameMarkerCommandEnabled,
	                               server->config.direct_gfx) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_SurfaceFrameMarkerEnabled,
	                               server->config.direct_gfx) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_HasExtendedMouseEvent, TRUE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_HasHorizontalWheel, FALSE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_HasRelativeMouseEvent, TRUE) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, server->config.width) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, server->config.height) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_MultifragMaxRequestSize, 0xFFFFFFU))
		return false;

	peer->PostConnect = peer_post_connect;
	peer->context->input->SynchronizeEvent = on_synchronize;
	peer->context->input->KeyboardEvent = on_keyboard;
	peer->context->input->UnicodeKeyboardEvent = on_unicode_keyboard;
	peer->context->input->MouseEvent = on_mouse;
	peer->context->input->RelMouseEvent = on_relative_mouse;
	peer->context->input->ExtendedMouseEvent = on_extended_mouse;
	peer->context->input->FocusInEvent = on_focus_in;
	return peer->Initialize(peer) == TRUE;
}

static DWORD WINAPI peer_thread(LPVOID argument)
{
	freerdp_peer* peer = (freerdp_peer*)argument;
	Server* server = (Server*)peer->ContextExtra;
	if (!configure_peer(peer, server))
		goto out;

	Client* client = (Client*)peer->context;
	log_message("INFO", "TLS RDP client 연결 수락");
	while (!client_should_stop(client))
	{
		if (client->bitmap_fallback_active && !client_flush_pending_bitmap(client))
			break;
		HANDLE handles[MAX_EVENT_HANDLES] = WINPR_C_ARRAY_INIT;
		DWORD count = peer->GetEventHandles(peer, handles, ARRAYSIZE(handles));
		if (count == 0 || count >= ARRAYSIZE(handles))
			break;
		if (client->direct_gfx_active)
			handles[count++] = WTSVirtualChannelManagerGetEventHandle(client->vcm);
		if (client->bitmap_fallback_active && client->bitmap_ready_event)
			handles[count++] = client->bitmap_ready_event;
		DWORD timeout = 20;
		if (client->bitmap_fallback_active)
		{
			EnterCriticalSection(&client->lock);
			const bool bitmap_pending = client->bitmap_queue_count > 0;
			const uint64_t last_send_started_at = client->bitmap_last_send_started_at;
			LeaveCriticalSection(&client->lock);
			if (bitmap_pending)
			{
				const uint64_t now = monotonic_milliseconds();
				if (last_send_started_at == 0 ||
				    now - last_send_started_at >= BITMAP_FRAME_INTERVAL_MS)
					timeout = 0;
				else
					timeout = WINPR_ASSERTING_INT_CAST(
					    DWORD, BITMAP_FRAME_INTERVAL_MS - (now - last_send_started_at));
			}
		}
		const DWORD status = WaitForMultipleObjects(count, handles, FALSE, timeout);
		if (status == WAIT_TIMEOUT)
		{
			if (client->direct_gfx_active &&
			    (!client_process_dynamic_channels(client) || !client_check_gfx_timeout(client)))
				break;
			continue;
		}
		if (status == WAIT_FAILED)
			break;
		if (!peer->CheckFileDescriptor(peer))
			break;
		if (client->bitmap_fallback_active && !client_flush_pending_bitmap(client))
			break;
		if (client->direct_gfx_active &&
		    (!client_process_dynamic_channels(client) || !client_check_gfx_timeout(client)))
			break;
	}
out:
	if (peer->Disconnect)
		peer->Disconnect(peer);
	freerdp_peer_context_free(peer);
	freerdp_peer_free(peer);
	return 0;
}

static BOOL peer_accepted(freerdp_listener* listener, freerdp_peer* peer)
{
	Server* server = (Server*)listener->info;
	EnterCriticalSection(&server->lock);
	const bool busy = server->active != NULL;
	LeaveCriticalSection(&server->lock);
	if (busy)
	{
		log_message("WARN", "single-client 제한으로 새 RDP 연결을 listener에서 거부합니다");
		return FALSE;
	}
	peer->ContextExtra = server;
	HANDLE thread = CreateThread(NULL, 0, peer_thread, peer, 0, NULL);
	if (!thread)
		return FALSE;
	(void)CloseHandle(thread);
	return TRUE;
}

static void print_usage(const char* executable)
{
	(void)fprintf(stderr,
	              "Usage: %s [-listen host:port] [-cert file] [-key file] [-width n] [-height n] "
	              "[-bitrate n] [-control-port n] [-video-port n] [-swap-alt-command] "
	              "[-right-alt-as-capslock] [-direct-gfx]\n",
	              executable);
}

static bool parse_listen(const char* value, const char** host, uint16_t* port)
{
	const char* separator = strrchr(value, ':');
	if (!separator || separator == value || separator[1] == '\0')
		return false;
	char* end = NULL;
	const long parsed = strtol(separator + 1, &end, 10);
	if (*end != '\0' || parsed < 1 || parsed > UINT16_MAX)
		return false;
	static char address[64];
	const size_t host_length = (size_t)(separator - value);
	if (host_length >= sizeof(address))
		return false;
	memcpy(address, value, host_length);
	address[host_length] = '\0';
	*host = address;
	*port = (uint16_t)parsed;
	return true;
}

int main(int argc, char* argv[])
{
	Server server = WINPR_C_ARRAY_INIT;
	server.config.bind_address = "0.0.0.0";
	server.config.port = 3389;
	server.config.certificate = "/root/nanokvm-rdp/cert.pem";
	server.config.private_key = "/root/nanokvm-rdp/key.pem";
	server.config.width = DEFAULT_WIDTH;
	server.config.height = DEFAULT_HEIGHT;
	server.config.bitrate = DEFAULT_BITRATE;
	server.config.control_port = 3390;
	server.config.video_port = 5004;
	server.control_fd = -1;
	server.control_listener = -1;
	for (int index = 1; index < argc; index++)
	{
		if (strcmp(argv[index], "-listen") == 0 && index + 1 < argc)
		{
			if (!parse_listen(argv[++index], &server.config.bind_address, &server.config.port))
				return 2;
		}
		else if (strcmp(argv[index], "-cert") == 0 && index + 1 < argc)
			server.config.certificate = argv[++index];
		else if (strcmp(argv[index], "-key") == 0 && index + 1 < argc)
			server.config.private_key = argv[++index];
		else if (strcmp(argv[index], "-width") == 0 && index + 1 < argc)
			server.config.width = (uint16_t)strtoul(argv[++index], NULL, 10);
		else if (strcmp(argv[index], "-height") == 0 && index + 1 < argc)
			server.config.height = (uint16_t)strtoul(argv[++index], NULL, 10);
		else if (strcmp(argv[index], "-bitrate") == 0 && index + 1 < argc)
			server.config.bitrate = (uint16_t)strtoul(argv[++index], NULL, 10);
		else if (strcmp(argv[index], "-control-port") == 0 && index + 1 < argc)
			server.config.control_port = (uint16_t)strtoul(argv[++index], NULL, 10);
		else if (strcmp(argv[index], "-video-port") == 0 && index + 1 < argc)
			server.config.video_port = (uint16_t)strtoul(argv[++index], NULL, 10);
		else if (strcmp(argv[index], "-swap-alt-command") == 0)
			server.config.swap_alt_command = true;
		else if (strcmp(argv[index], "-right-alt-as-capslock") == 0)
			server.config.right_alt_as_capslock = true;
		else if (strcmp(argv[index], "-direct-gfx") == 0)
			server.config.direct_gfx = true;
		else
		{
			print_usage(argv[0]);
			return 2;
		}
	}
	if (server.config.width == 0 || server.config.height == 0 || server.config.bitrate == 0 ||
	    server.config.control_port == 0 || server.config.video_port == 0)
		return 2;
	if (!InitializeCriticalSectionAndSpinCount(&server.lock, 4000) ||
	    !InitializeCriticalSectionAndSpinCount(&server.control_lock, 4000))
		return 1;
	if (!WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi()) ||
	    !winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT))
		return 1;

	freerdp_listener* listener = freerdp_listener_new();
	if (!listener)
		return 1;
	server.control_listener = open_control_listener(server.config.bind_address, server.config.control_port);
	if (server.control_listener < 0)
	{
		freerdp_listener_free(listener);
		return 1;
	}
	HANDLE control = CreateThread(NULL, 0, control_thread, &server, 0, NULL);
	if (!control)
	{
		(void)close(server.control_listener);
		freerdp_listener_free(listener);
		return 1;
	}
	(void)CloseHandle(control);
	listener->info = &server;
	listener->PeerAccepted = peer_accepted;
	WSADATA wsa = WINPR_C_ARRAY_INIT;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0 ||
	    !listener->Open(listener, server.config.bind_address, server.config.port))
	{
		freerdp_listener_free(listener);
		return 1;
	}
	(void)signal(SIGINT, on_signal);
	(void)signal(SIGTERM, on_signal);
	(void)signal(SIGPIPE, SIG_IGN);
	(void)fprintf(stderr, "%s: INFO: TLS RDP server listening on %s:%u\n", TAG,
	              server.config.bind_address, server.config.port);

	while (!stop_requested)
	{
		server_heartbeat(&server);
		HANDLE handles[8] = WINPR_C_ARRAY_INIT;
		const DWORD count = listener->GetEventHandles(listener, handles, ARRAYSIZE(handles));
		if (count == 0 || WaitForMultipleObjects(count, handles, FALSE, 200) == WAIT_FAILED)
			break;
		if (!listener->CheckFileDescriptor(listener))
			break;
	}
	listener->Close(listener);
	freerdp_listener_free(listener);
	if (server.control_fd >= 0)
		(void)close(server.control_fd);
	if (server.control_listener >= 0)
		(void)close(server.control_listener);
	WSACleanup();
	DeleteCriticalSection(&server.control_lock);
	DeleteCriticalSection(&server.lock);
	return 0;
}
