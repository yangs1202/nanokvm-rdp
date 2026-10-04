#include "bitmap_diff.h"
#include "agent_transport.h"
#include "device_session.h"
#include "ffmpeg_decoder.h"
#include "frame_flow.h"
#include "frame_trace.h"
#include "h264.h"
#include "hid.h"
#include "input_router.h"
#include "protocol.h"
#include "video_source.h"

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
#define BITMAP_FRAME_INTERVAL_MS 10U
#define GATEWAY_KEEPALIVE_INTERVAL_MS 10000U
#define CLASSIC_TILE_WIDTH 64U
#define CLASSIC_TILE_HEIGHT 64U
#define CLASSIC_TILE_MAX_ENCODED (CLASSIC_TILE_WIDTH * CLASSIC_TILE_HEIGHT * 4U)
#define CLASSIC_BITMAP_BATCH 1U
#define CLASSIC_MAX_UPDATE_SIZE (32U * 1024U)
#define STATS_LOG_INTERVAL_MS 5000U
#define PEER_SHUTDOWN_JOIN_TIMEOUT_MS 5000U
#define WHEEL_ROTATION_MASK 0x01FFU

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
	bool direct_gfx;
	bool swap_alt_command;
} ServerConfig;

typedef struct Server Server;
typedef struct Client Client;

struct Server
{
	ServerConfig config;
	CRITICAL_SECTION lock;
	int control_listener;
	HANDLE control_thread;
	HANDLE peer_thread;
	AgentTransport transport;
	DeviceSession* device_session;
	uint64_t last_stats_log_at;
	uint64_t next_keepalive_at;
	int8_t keepalive_dx;
	Client* active;
	bool active_closing;
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
	AVFrame* pending_decoded;
	FfmpegConverter converter;
	uint32_t converted_frames;
	uint8_t* previous_bitmap;
	size_t previous_bitmap_length;
	uint8_t* classic_encoded;
	bool previous_bitmap_valid;
	HidState hid;
	HANDLE shutdown_event;
	bool stopping;
	bool owns_active_client;
	bool direct_gfx_active;
	bool bitmap_fallback_active;
	bool gfx_ready;
	bool gfx_opened;
	FrameFlow frame_flow;
	FrameTrace frame_trace;
	bool need_idr;
	uint64_t gfx_wait_started_at;
	uint64_t gfx_opened_at;
	uint32_t next_frame_id;
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
	bool pointer_position_logged;
	bool wheel_input_logged;
	InputRouter input_router;
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
	(void)fprintf(stderr, "%s: %s: at_ms=%llu %s\n", TAG, level,
	              (unsigned long long)monotonic_milliseconds(), message);
}

static bool server_send_control(Server* server, uint8_t type, const void* payload, uint16_t length)
{
	return agent_transport_send(&server->transport, type, payload, length);
}

/* Keep the remote host awake from the gateway even when the RDP client is
 * disconnected. Never inject motion while the RDP client holds a button or
 * modifier, because a relative report with no button state would release it. */
static void server_keepalive(Server* server, uint64_t now)
{
	if (now < server->next_keepalive_at)
		return;

	server->next_keepalive_at = now + GATEWAY_KEEPALIVE_INTERVAL_MS;
	bool idle = true;
	EnterCriticalSection(&server->lock);
	Client* active = server->active;
	if (active)
	{
		EnterCriticalSection(&active->lock);
		idle = active->input_router.pointer_buttons == 0 &&
		       active->input_router.keyboard_modifiers == 0 &&
		       !active->input_router.control_space_down;
		LeaveCriticalSection(&active->lock);
	}
	LeaveCriticalSection(&server->lock);
	if (!idle)
		return;

	uint8_t payload[5] = { 0 };
	protocol_write_u16(payload, (uint16_t)(int16_t)server->keepalive_dx);
	/* The gateway keepalive is a one-unit horizontal relative motion with no
	 * buttons or wheel. The agent applies it to the USB HID mouse endpoint. */
	if (server_send_control(server, NANOKVM_CONTROL_POINTER_REL, payload, sizeof(payload)))
		server->keepalive_dx = (int8_t)-server->keepalive_dx;
}

static bool server_set_stream_requested(Server* server, bool requested)
{
	const bool sent = agent_transport_set_stream_requested(&server->transport, requested);
	log_message(sent ? "INFO" : "ERROR", requested ? "NanoKVM agent에 START_STREAM 전송"
	                                                : "NanoKVM agent에 STOP_STREAM 전송");
	return sent;
}

static bool request_video_idr(void* context, uint64_t video_source_epoch)
{
	/* This epoch identifies the successful RTP open that produced the access
	 * unit. It is intentionally independent of AgentTransport's control
	 * connection epoch; server_send_control preserves the existing current-
	 * transport-epoch validation when it sends IDR_REQUEST. */
	(void)video_source_epoch;
	return server_send_control((Server*)context, NANOKVM_CONTROL_IDR_REQUEST, NULL, 0);
}

static void on_signal(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

/* The peer thread exclusively performs FreeRDP teardown. Keep a running handle
 * until shutdown join; completed handles are reaped by listener admission so a
 * new peer can be accepted without outliving Server-owned state. */
static bool server_join_peer_thread(Server* server)
{
	HANDLE thread = NULL;
	EnterCriticalSection(&server->lock);
	if (server->active && server->active->shutdown_event)
		(void)SetEvent(server->active->shutdown_event);
	thread = server->peer_thread;
	LeaveCriticalSection(&server->lock);
	if (!thread)
		return true;

	const DWORD status = WaitForSingleObject(thread, PEER_SHUTDOWN_JOIN_TIMEOUT_MS);
	if (status != WAIT_OBJECT_0)
	{
		log_message("ERROR", "RDP peer thread가 종료 시간 제한 내에 정리되지 않았습니다");
		return false;
	}

	EnterCriticalSection(&server->lock);
	if (server->peer_thread == thread)
		server->peer_thread = NULL;
	LeaveCriticalSection(&server->lock);
	(void)CloseHandle(thread);
	return true;
}

/* Listener callbacks run on the main thread. Reap a completed peer handle before
 * admission, while leaving a running handle as an explicit single-client gate. */
static void server_reap_finished_peer_thread(Server* server)
{
	HANDLE thread = NULL;
	EnterCriticalSection(&server->lock);
	if (server->peer_thread && WaitForSingleObject(server->peer_thread, 0) == WAIT_OBJECT_0)
	{
		thread = server->peer_thread;
		server->peer_thread = NULL;
	}
	LeaveCriticalSection(&server->lock);
	if (thread)
	{
		(void)CloseHandle(thread);
		log_message("INFO", "종료된 RDP peer thread handle을 재사용 가능 상태로 회수했습니다");
	}
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
		const struct timeval send_timeout = { .tv_usec = 100000 };
		(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
		uint64_t epoch = 0;
		if (!agent_transport_accept(&server->transport, fd, &epoch))
			continue;
		log_message("INFO", "NanoKVM agent control 연결 수락");
		agent_transport_run(&server->transport, fd, epoch);
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

static uint64_t transport_clock(void* context)
{
	(void)context;
	return monotonic_milliseconds();
}

static void on_transport_event(void* context, AgentTransportEvent event, uint64_t epoch)
{
	Server* server = (Server*)context;
	if (event == AGENT_TRANSPORT_EVENT_DISCONNECTED)
	{
		log_message("WARN", "NanoKVM agent control 연결 종료");
		return;
	}
	if (!agent_transport_is_current_epoch(&server->transport, epoch))
		return;

	bool requested = false;
	/* The peer thread owns FreeRDP teardown and observes this request before Disconnect. */
	EnterCriticalSection(&server->lock);
	if (server->active && server->active->shutdown_event)
	{
		(void)SetEvent(server->active->shutdown_event);
		requested = true;
	}
	LeaveCriticalSection(&server->lock);
	if (requested)
		log_message("WARN", "NanoKVM agent heartbeat timeout; active RDP client 연결을 종료합니다");
	else
		log_message("WARN", "NanoKVM agent heartbeat timeout; control 연결을 종료합니다");
}

static void server_heartbeat(Server* server)
{
	const uint64_t now = monotonic_milliseconds();
	agent_transport_heartbeat(&server->transport, now);
	server_keepalive(server, now);
	if (now - server->last_stats_log_at < STATS_LOG_INTERVAL_MS)
		return;
	server->last_stats_log_at = now;
	struct rusage usage = { 0 };
	if (getrusage(RUSAGE_SELF, &usage) == 0)
	{
		AgentTransportStats agent_stats = { 0 };
		agent_transport_get_stats(&server->transport, &agent_stats);
		uint32_t bitmap_frames = 0;
		uint32_t decoded_frames = 0;
		uint32_t converted_frames = 0;
		unsigned interval_ms = 0;
		uint64_t decode_ms = 0;
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
			converted_frames = active->converted_frames;
			interval_ms = frame_flow_interval(&active->frame_flow);
			decode_ms = active->last_decode_latency_ms;
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
			bitmap_pending = active->pending_decoded != NULL;
			rdp_send_ms = active->last_rdp_send_ms;
			LeaveCriticalSection(&active->lock);
		}
		LeaveCriticalSection(&server->lock);
		char message[512] = { 0 };
		(void)snprintf(message, sizeof(message),
			               "STATS agent packets=%u dropped=%u frames=%u dropped_frames=%u rtp_nals=%u au=%u idr=%u p=%u decoded=%u converted=%u interval_ms=%u queued=%u queue_drop=%u stale_drop=%u flush=%u empty_flush=%u rdp_frames=%u classic_tiles=%llu classic_bytes=%llu queue=%u gateway_rss=%ld decode_ms=%llu rdp_send_ms=%llu",
			               agent_stats.sent_packets, agent_stats.dropped_packets,
			               agent_stats.capture_frames, agent_stats.dropped_frames,
			               rtp_nals, rtp_access_units, rtp_idr_units, rtp_p_units, decoded_frames, converted_frames, interval_ms,
			               bitmap_queued_frames, bitmap_queue_drops, bitmap_stale_drops, bitmap_flushes,
			               bitmap_empty_flushes,
			               bitmap_frames, (unsigned long long)classic_tiles_sent,
			               (unsigned long long)classic_bytes_sent, bitmap_pending ? 1U : 0U,
		               usage.ru_maxrss, (unsigned long long)decode_ms,
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
	/* Temporary macOS Windows App comparison: bypass the iOS compatibility
	 * heuristic, but still require advertised AVC support for the selected cap. */

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
	uint64_t elapsed = 0;
	EnterCriticalSection(&client->lock);
	bool matched = frame_flow_ack(&client->frame_flow, acknowledge->frameId,
	                                   acknowledge->queueDepth, monotonic_milliseconds(), &elapsed);
	FrameTraceEntry trace = { 0 };
	const uint64_t acknowledged_at = monotonic_milliseconds();
	if (!client->gfx_uses_progressive)
	{
		matched = frame_trace_ack(&client->frame_trace, acknowledge->frameId, acknowledged_at, &trace);
		elapsed = matched ? acknowledged_at - trace.sent_at : 0;
	}
	const unsigned pending = client->frame_flow.count;
	LeaveCriticalSection(&client->lock);
	char message[192];
	(void)snprintf(message, sizeof(message),
	               "RDP frame ACK id=%u decoded=%u queue_bytes=%u pending=%u matched=%u elapsed_ms=%llu",
	               acknowledge->frameId, acknowledge->totalFramesDecoded, acknowledge->queueDepth,
	               pending, (unsigned)matched, (unsigned long long)elapsed);
	log_message(elapsed >= 200U ? "WARN" : "INFO", message);
	if (trace.valid && frame_trace_sample(trace.rtp_timestamp))
	{
		char detail[256];
		(void)snprintf(detail, sizeof(detail),
		               "FRAME_ACK ssrc=%u rtp_ts=%u frame_id=%u ack_ms=%llu send_to_ack_ms=%llu receive_to_ack_ms=%llu queue_bytes=%u",
		               trace.ssrc, trace.rtp_timestamp, trace.frame_id,
		               (unsigned long long)acknowledged_at, (unsigned long long)elapsed,
		               (unsigned long long)(acknowledged_at - trace.received_at), acknowledge->queueDepth);
		log_message("INFO", detail);
	}
	if (client->bitmap_ready_event) (void)SetEvent(client->bitmap_ready_event);
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

static bool send_avc420_frame(Client* client, const uint8_t* data, size_t length,
                              uint32_t ssrc, uint32_t rtp_timestamp, uint64_t received_at,
                              uint64_t receive_wall)
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

	const uint64_t send_started = monotonic_milliseconds();
	EnterCriticalSection(&client->lock);
	frame_trace_sent(&client->frame_trace, start.frameId, ssrc, rtp_timestamp, received_at, send_started);
	LeaveCriticalSection(&client->lock);
	if (!client->gfx || !client->gfx->SurfaceFrameCommand)
		error = ERROR_INVALID_HANDLE;
	else
		error = client->gfx->SurfaceFrameCommand(client->gfx, &command, &start, &end);
	const uint64_t send_done = monotonic_milliseconds();
	EnterCriticalSection(&client->lock);
	client->last_rdp_send_ms = send_done - send_started;
	LeaveCriticalSection(&client->lock);
	if (frame_trace_sample(rtp_timestamp))
	{
		char detail[384];
		(void)snprintf(detail, sizeof(detail),
		               "FRAME_SEND ssrc=%u rtp_ts=%u frame_id=%u receive_ms=%llu receive_wall_ms=%llu send_start_ms=%llu send_done_ms=%llu receive_to_send_ms=%llu submit_ms=%llu bytes=%zu ok=%u",
		               ssrc, rtp_timestamp, start.frameId, (unsigned long long)received_at,
		               (unsigned long long)receive_wall, (unsigned long long)send_started,
		               (unsigned long long)send_done, (unsigned long long)(send_started - received_at),
		               (unsigned long long)(send_done - send_started), length, error == CHANNEL_RC_OK);
		log_message("INFO", detail);
	}
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
	DeviceSessionLease lease = { 0 };
	if (!device_session_acquire(client->server->device_session,
	                            DEVICE_SESSION_CONSUMER_DIRECT_GFX, &lease))
	{
		log_message("ERROR", "RDPGFX RTP/H.264 receiver를 시작할 수 없습니다");
		client_stop(client);
		return 0;
	}
	log_message("INFO", "RDPGFX RTP/H.264 passthrough 시작");
	char buffer_message[128];
	(void)snprintf(buffer_message, sizeof(buffer_message), "RTP receive buffer bytes=%d",
	               device_session_receive_buffer_bytes(&lease));
	log_message("INFO", buffer_message);
	while (!client_should_stop(client))
	{
		VideoSourceAccessUnit access_unit = { 0 };
		if (!device_session_read(&lease, &access_unit))
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
		uint8_t* data = access_unit.data;
		const size_t length = access_unit.length;
		if (!data || length == 0)
		{
			video_source_release_access_unit(&access_unit);
			continue;
		}
		const uint64_t received_at = monotonic_milliseconds();
		const uint64_t receive_wall = frame_trace_wall_ms();
		if (access_unit.packet_loss)
		{
			EnterCriticalSection(&client->lock);
			client->need_idr = true;
			LeaveCriticalSection(&client->lock);
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
			if (!payload_ok || !send_avc420_frame(client, payload, payload_length,
			                                          access_unit.ssrc, access_unit.timestamp,
			                                          received_at, receive_wall))
				client_stop(client);
			free(owned);
		}
		video_source_release_access_unit(&access_unit);
	}
	device_session_release(&lease);
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
			    !bitmap_tile_changed(client->previous_bitmap, bgra, width, left, top, columns, rows))
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
	RDPGFX_SURFACE_COMMAND command = WINPR_C_ARRAY_INIT;
	RDPGFX_START_FRAME_PDU start = WINPR_C_ARRAY_INIT;
	RDPGFX_END_FRAME_PDU end = WINPR_C_ARRAY_INIT;

	if (!client->progressive || !client->gfx || !client->gfx->SurfaceFrameCommand ||
	    length != expected_length || client_should_stop(client))
		return false;
	region16_init(&region);
	const uint16_t tile_columns = (uint16_t)((width + 63U) / 64U);
	const uint16_t tile_rows = (uint16_t)((height + 63U) / 64U);
	if (!client->previous_bitmap)
	{
		client->previous_bitmap = malloc(expected_length);
		client->previous_bitmap_length = client->previous_bitmap ? expected_length : 0;
	}
	const uint8_t* previous = client->previous_bitmap_valid &&
	                          client->previous_bitmap_length == expected_length
	                              ? client->previous_bitmap
	                              : NULL;
	size_t changed_tiles = 0;
	for (uint16_t tile_y = 0; tile_y < tile_rows; tile_y++)
	{
		for (uint16_t tile_x = 0; tile_x < tile_columns; tile_x++)
		{
			const uint16_t left = (uint16_t)(tile_x * 64U);
			const uint16_t top = (uint16_t)(tile_y * 64U);
			const uint16_t columns = (uint16_t)(left + 64U > width ? (uint16_t)(width - left) : 64U);
			const uint16_t rows = (uint16_t)(top + 64U > height ? (uint16_t)(height - top) : 64U);
			if (previous && !bitmap_tile_changed(previous, bgra, width, left, top, columns, rows))
				continue;
			RECTANGLE_16 rect = { .left = left, .top = top,
				.right = (UINT16)(left + columns), .bottom = (UINT16)(top + rows) };
			if (!region16_union_rect(&region, &region, &rect))
			{
				region16_uninit(&region);
				return false;
			}
			changed_tiles++;
		}
	}
	if (changed_tiles == 0)
	{
		region16_uninit(&region);
		return true;
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
	/* Register before submission: the channel receiver may ACK concurrently. */
	EnterCriticalSection(&client->lock);
	const bool tracked = frame_flow_sent(&client->frame_flow, start.frameId,
	                                     monotonic_milliseconds());
	LeaveCriticalSection(&client->lock);
	if (!tracked) return false;
	const UINT error = client->gfx->SurfaceFrameCommand(client->gfx, &command, &start, &end);
	command.data = NULL;
	if (error != CHANNEL_RC_OK)
	{
		log_message("ERROR", "RDPGFX Progressive frame 전송 실패");
		return false;
	}
	if (client->previous_bitmap && client->previous_bitmap_length == expected_length)
	{
		memcpy(client->previous_bitmap, bgra, expected_length);
		client->previous_bitmap_valid = true;
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

/* The decoder owns its frame; retain the reference, not an 8 MB BGRA copy. */
static bool on_decoded_frame(void* context, const AVFrame* frame)
{
	Client* client = context;
	if (client_should_stop(client)) return false;
	AVFrame* retained = av_frame_clone(frame);
	if (!retained) return false;
	EnterCriticalSection(&client->lock);
	AVFrame* stale = client->pending_decoded;
	client->pending_decoded = retained;
	client->decoded_frames++;
	client->bitmap_queued_frames++;
	if (stale)
	{
		client->bitmap_queue_drops++;
		client->bitmap_stale_drops++;
	}
	client->last_decode_latency_ms = monotonic_milliseconds() - client->last_rtp_received_at;
	LeaveCriticalSection(&client->lock);
	av_frame_free(&stale);
	if (client->bitmap_ready_event) (void)SetEvent(client->bitmap_ready_event);
	return true;
}

static bool client_flush_pending_bitmap(Client* client)
{
	if (client->bitmap_ready_event) (void)ResetEvent(client->bitmap_ready_event);
	if (client->peer && client->peer->IsWriteBlocked && client->peer->DrainOutputBuffer &&
	    client->peer->IsWriteBlocked(client->peer))
	{
		(void)client->peer->DrainOutputBuffer(client->peer);
		if (client->peer->IsWriteBlocked(client->peer)) return true;
	}
	const uint64_t now = monotonic_milliseconds();
	EnterCriticalSection(&client->lock);
	client->bitmap_flushes++;
	const unsigned interval = client->gfx_uses_progressive
	                              ? frame_flow_interval(&client->frame_flow) : BITMAP_FRAME_INTERVAL_MS;
	if ((client->gfx_uses_progressive && frame_flow_blocked(&client->frame_flow)) ||
	    (client->bitmap_last_send_started_at && now - client->bitmap_last_send_started_at < interval))
	{
		LeaveCriticalSection(&client->lock);
		return true;
	}
	AVFrame* frame = client->pending_decoded;
	if (!frame)
	{
		client->bitmap_empty_flushes++;
		LeaveCriticalSection(&client->lock);
		return true;
	}
	/* Keep replacing the pending raw frame while blocked. Even when capture
	 * stops, the final frame survives until pacing/ACK/transport allows it. */
	client->pending_decoded = NULL;
	client->bitmap_last_send_started_at = now;
	LeaveCriticalSection(&client->lock);

	const bool converted = ffmpeg_converter_convert(&client->converter, frame,
	                                                client->render_width, client->render_height);
	av_frame_free(&frame);
	const bool sent = converted && send_bitmap_frame(client, client->converter.frame,
	                                                client->converter.frame_size);
	const uint64_t completed_at = monotonic_milliseconds();
	if (completed_at - now >= 100U)
	{
		char message[128];
		(void)snprintf(message, sizeof(message), "RDP slow frame send elapsed_ms=%llu ok=%u",
		               (unsigned long long)(completed_at - now), (unsigned)sent);
		log_message("WARN", message);
	}
	EnterCriticalSection(&client->lock);
	if (converted) client->converted_frames++;
	if (sent) client->last_rdp_send_ms = completed_at - now;
	if (client->gfx_uses_progressive)
		frame_flow_send_cost(&client->frame_flow, completed_at - now, completed_at);
	LeaveCriticalSection(&client->lock);
	if (!sent)
	{
		log_message("ERROR", "RDP bitmap 변환 또는 전송 실패");
		client_stop(client);
	}
	return sent;
}

static DWORD WINAPI bitmap_video_thread(LPVOID argument)
{
	Client* client = (Client*)argument;
	DeviceSessionLease lease = { 0 };
	FfmpegDecoder decoder = { 0 };
	bool backend_ready = false;

	for (unsigned attempt = 0; attempt < 10 && !client_should_stop(client); attempt++)
	{
		if (device_session_acquire(client->server->device_session,
		                           DEVICE_SESSION_CONSUMER_BITMAP, &lease) &&
		    ffmpeg_decoder_start_raw(&decoder, on_decoded_frame, client))
		{
			backend_ready = true;
			break;
		}
		ffmpeg_decoder_stop(&decoder);
		device_session_release(&lease);
		Sleep(1000);
	}
	if (!backend_ready)
	{
		log_message("ERROR", "RTP/H.264 receiver 또는 FFmpeg decoder를 시작할 수 없습니다");
		client_stop(client);
		goto out;
	}
	log_message("INFO", "RTP/H.264 receiver와 FFmpeg BGRA decoder 시작 완료");
	while (!client_should_stop(client))
	{
		VideoSourceAccessUnit access_unit = { 0 };
		if (!device_session_read(&lease, &access_unit))
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
		uint8_t* data = access_unit.data;
		const size_t length = access_unit.length;
		if (!data || length == 0)
		{
			video_source_release_access_unit(&access_unit);
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
			video_source_release_access_unit(&access_unit);
			client_stop(client);
			break;
		}
		(void)h264_copy_annexb(annexb, data, length);
		const bool packet_loss = access_unit.packet_loss;
		const bool pushed = ffmpeg_decoder_push(&decoder, annexb, annexb_length);
		free(annexb);
		video_source_release_access_unit(&access_unit);
		if (!pushed)
		{
			log_message("ERROR", "FFmpeg H.264 decoder 입력 실패");
			client_stop(client);
			break;
		}
		if (packet_loss)
		{
			log_message("WARN", "RTP frame loss 감지; NanoKVM agent에 IDR 재동기화를 요청합니다");
		}
	}

out:
	ffmpeg_decoder_stop(&decoder);
	device_session_release(&lease);
	return 0;
}

static bool client_send_input_message(Client* client, const InputRouterMessage* message)
{
	return message->type == 0 ||
	       server_send_control(client->server, message->type, message->payload, message->length);
}

/* Keep the RDP client's cursor independent from the captured host video. The
 * host cursor will eventually appear in a video frame, but that frame can be
 * delayed by mobile decoder/backpressure. RDP has a small standalone pointer
 * position update for exactly this case. */
static void client_send_pointer_position(Client* client, UINT16 x, UINT16 y)
{
	rdpUpdate* update = client->context.update;
	if (!update || !update->pointer || !update->pointer->PointerPosition)
		return;
	if (!client->pointer_position_logged && update->pointer->PointerSystem)
	{
		POINTER_SYSTEM_UPDATE pointer_system = { .type = SYSPTR_DEFAULT };
		(void)update->pointer->PointerSystem(&client->context, &pointer_system);
	}
	POINTER_POSITION_UPDATE position = {
		.xPos = hid_clamp_absolute(x, client->render_width),
		.yPos = hid_clamp_absolute(y, client->render_height),
	};
	if (!update->pointer->PointerPosition(&client->context, &position))
		return;
	if (!client->pointer_position_logged)
	{
		log_message("INFO", "RDP pointer position fast path 활성화");
		client->pointer_position_logged = true;
	}
}

static bool input_trace_enabled(void)
{
	const char* value = getenv("NANOKVM_INPUT_TRACE");
	return value && (strcmp(value, "1") == 0 || strcmp(value, "shift") == 0);
}

static bool input_trace_scancode(uint8_t code, bool extended)
{
	/* Keep the opt-in trace limited to the Shift+a/Shift+1 diagnostic. */
	return !extended && (code == 0x02 || code == 0x1e || code == 0x2a || code == 0x36);
}

static BOOL on_keyboard(rdpInput* input, UINT16 flags, UINT8 code)
{
	Client* client = (Client*)input->context;
	const bool raw_extended = (flags & KBD_FLAGS_EXTENDED) != 0;
	const bool release = (flags & KBD_FLAGS_RELEASE) != 0;
	InputRouterMessage message;
	uint8_t modifiers_before = 0;
	uint8_t modifiers_after = 0;
	bool control_space_down = false;
	EnterCriticalSection(&client->lock);
	modifiers_before = client->input_router.keyboard_modifiers;
	input_router_keyboard(&client->input_router, code, raw_extended, release, &message);
	modifiers_after = client->input_router.keyboard_modifiers;
	control_space_down = client->input_router.control_space_down;
	LeaveCriticalSection(&client->lock);
	if (modifiers_after != 0 || code == 0x3a)
	{
		char diagnostic[192];
		(void)snprintf(diagnostic, sizeof(diagnostic),
		               "RDP modifier diagnostic raw=0x%02X extended=%u release=%u mapped=0x%02X extended=%u modifiers=0x%02X",
		               code, (unsigned)raw_extended, (unsigned)release,
		               message.payload[0], (unsigned)message.payload[1],
		               modifiers_after);
		log_message("INFO", diagnostic);
	}
	if (code == 0x39 && !raw_extended &&
	    ((modifiers_after & 0x11U) || control_space_down))
	{
		char diagnostic[128];
		(void)snprintf(diagnostic, sizeof(diagnostic),
		               "RDP Control+Space diagnostic release=%u modifiers=0x%02X",
		               (unsigned)release, modifiers_after);
		log_message("INFO", diagnostic);
	}
	const bool input_trace = input_trace_enabled() && input_trace_scancode(code, raw_extended);
	uint64_t input_trace_epoch = 0;
	if (input_trace)
		input_trace_epoch = agent_transport_current_epoch(&client->server->transport);
	const bool sent = client_send_input_message(client, &message);
	if (input_trace)
	{
		char diagnostic[256];
		(void)snprintf(diagnostic, sizeof(diagnostic),
		               "INPUT TRACE raw=0x%02X extended=%u release=%u modifiers_before=0x%02X modifiers_after=0x%02X payload=0x%02X/%u/%u epoch=%llu sent=%u",
		               code, (unsigned)raw_extended, (unsigned)release, modifiers_before,
		               modifiers_after, message.payload[0],
		               (unsigned)message.payload[1], (unsigned)message.payload[2],
		               (unsigned long long)input_trace_epoch, (unsigned)sent);
		log_message("INFO", diagnostic);
	}
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
	const uint64_t received_at = monotonic_milliseconds();
	InputRouterMessage message;
	if (!input_router_unicode(&client->input_router, code, (flags & KBD_FLAGS_RELEASE) != 0, &message))
	{
		char diagnostic[128];
		(void)snprintf(diagnostic, sizeof(diagnostic),
		               "지원하지 않는 RDP Unicode keyboard code unit U+%04X", code);
		log_message("WARN", diagnostic);
		return FALSE;
	}
	if (message.type == 0)
		return TRUE;
	const bool sent = client_send_input_message(client, &message);
	char diagnostic[160];
	(void)snprintf(diagnostic, sizeof(diagnostic),
	               "RDP Unicode input received_ms=%llu ascii=%u bytes=%u forward_ms=%llu ok=%u",
	               (unsigned long long)received_at, (unsigned)(code <= 0x7fU), message.length,
	               (unsigned long long)(monotonic_milliseconds() - received_at), (unsigned)sent);
	log_message("INFO", diagnostic);
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
	InputRouterMessage message;
	EnterCriticalSection(&client->lock);
	input_router_release_all(&client->input_router, &message);
	LeaveCriticalSection(&client->lock);
	const bool sent = client_send_input_message(client, &message);
	if (sent && reason)
	{
		char message[128];
		(void)snprintf(message, sizeof(message),
		               "RDP input state resync: %s → RELEASE_ALL 전송", reason);
		log_message("INFO", message);
	}
	return sent;
}

static BOOL on_synchronize(rdpInput* input, UINT32 toggle_states)
{
	Client* client = (Client*)input->context;
	char message[160];
	uint8_t previous_modifiers = 0;
	uint8_t previous_buttons = 0;
	EnterCriticalSection(&client->lock);
	previous_modifiers = client->input_router.keyboard_modifiers;
	previous_buttons = client->input_router.pointer_buttons;
	LeaveCriticalSection(&client->lock);
	(void)snprintf(message, sizeof(message),
	               "RDP Synchronize toggles=0x%08X previous_modifiers=0x%02X previous_buttons=0x%02X",
	               toggle_states, previous_modifiers, previous_buttons);
	log_message("INFO", message);
	/* TS_SYNC_EVENT resets held keys. Subsequent down events restore held keys;
	 * do not cancel preceding input that is still waiting for USB delivery. */
	InputRouterMessage output;
	EnterCriticalSection(&client->lock);
	input_router_synchronize(&client->input_router, &output);
	LeaveCriticalSection(&client->lock);
	return client_send_input_message(client, &output);
}

static BOOL on_mouse(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
{
	Client* client = (Client*)input->context;
	const bool has_wheel = (flags & (PTR_FLAGS_WHEEL | PTR_FLAGS_HWHEEL)) != 0U;
	InputRouterMessage message;
	uint8_t pointer_buttons = 0;
	uint8_t keyboard_modifiers = 0;
	EnterCriticalSection(&client->lock);
	input_router_absolute_pointer(&client->input_router, x, y, flags, has_wheel, &message);
	pointer_buttons = client->input_router.pointer_buttons;
	keyboard_modifiers = client->input_router.keyboard_modifiers;
	LeaveCriticalSection(&client->lock);
	if (!has_wheel)
		client_send_pointer_position(client, x, y);
	if ((flags & (PTR_FLAGS_BUTTON1 | PTR_FLAGS_BUTTON2 | PTR_FLAGS_BUTTON3)) != 0)
	{
		char message[160];
		(void)snprintf(message, sizeof(message),
		               "RDP absolute button flags=0x%04X x=%u y=%u down=%u mask=0x%02X modifiers=0x%02X",
		               flags, x, y, (unsigned)((flags & PTR_FLAGS_DOWN) != 0),
		               pointer_buttons, keyboard_modifiers);
		log_message("INFO", message);
	}
	/* 휠 비트만 있는 이벤트는 좌표가 0이다. 위치 보고와 분리해 커서가 원점으로 튀지 않게 한다. */
	const bool sent = client_send_input_message(client, &message);
	if (sent && !has_wheel && !client->pointer_input_logged)
	{
		log_message("INFO", "RDP absolute pointer → NanoKVM agent HID 전달 확인");
		client->pointer_input_logged = true;
	}
	if (sent && has_wheel &&
	    !client->wheel_input_logged)
	{
		log_message("INFO", "RDP wheel → NanoKVM agent HID 전달 확인");
		client->wheel_input_logged = true;
	}
	return sent;
}

static BOOL on_extended_mouse(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
{
	return on_mouse(input, hid_pointer_flags_from_extended(flags), x, y);
}

static BOOL on_relative_mouse(rdpInput* input, UINT16 flags, INT16 x_delta, INT16 y_delta)
{
	Client* client = (Client*)input->context;
	bool vertical_wheel = (flags & PTR_FLAGS_WHEEL) != 0;
	bool horizontal_wheel = (flags & PTR_FLAGS_HWHEEL) != 0;
	/* Windows App의 가로 스크롤은 상대 마우스의 가로 델타와 세로 휠 비트를 같이 보낸다.
	 * 휠 비트만 보면 페이지가 세로로만 움직이므로 가로 델타를 AC Pan으로 바꾼다. */
	if (!horizontal_wheel && vertical_wheel && x_delta != 0 && y_delta == 0)
	{
		flags = (uint16_t)((flags & (uint16_t)~(PTR_FLAGS_WHEEL | WHEEL_ROTATION_MASK)) |
		                   PTR_FLAGS_HWHEEL | (uint16_t)(flags & WHEEL_ROTATION_MASK));
		if (x_delta < 0)
			flags |= PTR_FLAGS_WHEEL_NEGATIVE;
		else
			flags = (uint16_t)(flags & (uint16_t)~PTR_FLAGS_WHEEL_NEGATIVE);
		horizontal_wheel = true;
	}
	const bool has_wheel = (flags & (PTR_FLAGS_WHEEL | PTR_FLAGS_HWHEEL)) != 0;
	InputRouterMessage message;
	uint8_t pointer_buttons = 0;
	uint8_t keyboard_modifiers = 0;
	EnterCriticalSection(&client->lock);
	input_router_relative_pointer(&client->input_router, x_delta, y_delta, flags, has_wheel, &message);
	pointer_buttons = client->input_router.pointer_buttons;
	keyboard_modifiers = client->input_router.keyboard_modifiers;
	LeaveCriticalSection(&client->lock);
	if ((flags & (PTR_FLAGS_BUTTON1 | PTR_FLAGS_BUTTON2 | PTR_FLAGS_BUTTON3)) != 0)
	{
		char message[192];
		(void)snprintf(message, sizeof(message),
		               "RDP relative button flags=0x%04X dx=%d dy=%d mask=0x%02X down=%u modifiers=0x%02X",
		               flags, x_delta, y_delta, pointer_buttons,
		               (unsigned)((flags & PTR_FLAGS_DOWN) != 0), keyboard_modifiers);
		log_message("INFO", message);
	}
	const bool sent = client_send_input_message(client, &message);
	if (sent && has_wheel &&
	    !client->wheel_input_logged)
	{
		log_message("INFO", "RDP relative wheel → NanoKVM agent HID 전달 확인");
		client->wheel_input_logged = true;
	}
	return sent;
}

static BOOL client_context_new(freerdp_peer* peer, rdpContext* context)
{
	Client* client = (Client*)context;
	Server* server = (Server*)peer->ContextExtra;
	if (!server || !InitializeCriticalSectionAndSpinCount(&client->lock, 4000))
		return FALSE;
	client->server = server;
	client->peer = peer;
	client->shutdown_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!client->shutdown_event)
		goto fail;
	client->need_idr = true;
	input_router_init(&client->input_router, server->config.width, server->config.height,
	                  server->config.swap_alt_command);
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
	if (server->active || server->active_closing)
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
	if (client->server)
	{
		EnterCriticalSection(&client->server->lock);
		if (client->owns_active_client && client->server->active == client)
		{
			client->server->active = NULL;
			client->server->active_closing = true;
		}
		HANDLE shutdown_event = client->shutdown_event;
		client->shutdown_event = NULL;
		LeaveCriticalSection(&client->server->lock);
		if (shutdown_event)
			(void)CloseHandle(shutdown_event);
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
	av_frame_free(&client->pending_decoded);
	ffmpeg_converter_free(&client->converter);
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
		client->server->active_closing = false;
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
	    !freerdp_settings_set_bool(settings, FreeRDP_HasHorizontalWheel, TRUE) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_KeyboardLayout, 0x00000412U) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_KeyboardType, 4) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_KeyboardSubType, 0) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_KeyboardFunctionKey, 12) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_HasRelativeMouseEvent, TRUE) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, server->config.width) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, server->config.height) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_MultifragMaxRequestSize, 0xFFFFFFU))
		return false;

	peer->PostConnect = peer_post_connect;
	peer->context->input->KeyboardEvent = on_keyboard;
	peer->context->input->UnicodeKeyboardEvent = on_unicode_keyboard;
	peer->context->input->MouseEvent = on_mouse;
	peer->context->input->RelMouseEvent = on_relative_mouse;
	peer->context->input->ExtendedMouseEvent = on_extended_mouse;
	peer->context->input->SynchronizeEvent = on_synchronize;
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
		HANDLE handles[MAX_EVENT_HANDLES] = WINPR_C_ARRAY_INIT;
		DWORD count = peer->GetEventHandles(peer, handles, ARRAYSIZE(handles));
		if (count == 0)
			break;
		if (client->direct_gfx_active)
		{
			if (count >= ARRAYSIZE(handles))
				break;
			handles[count++] = WTSVirtualChannelManagerGetEventHandle(client->vcm);
		}
		if (client->bitmap_fallback_active && client->bitmap_ready_event)
		{
			if (count >= ARRAYSIZE(handles))
				break;
			handles[count++] = client->bitmap_ready_event;
		}
		if (count >= ARRAYSIZE(handles))
			break;
		handles[count++] = client->shutdown_event;
		DWORD timeout = 20;
		if (client->bitmap_fallback_active)
		{
			EnterCriticalSection(&client->lock);
			const bool bitmap_pending = client->pending_decoded != NULL;
			const bool waiting_for_ack = client->gfx_uses_progressive &&
			                             frame_flow_blocked(&client->frame_flow);
			const uint64_t last_send_started_at = client->bitmap_last_send_started_at;
			const uint64_t interval = client->gfx_uses_progressive
			                              ? frame_flow_interval(&client->frame_flow) : BITMAP_FRAME_INTERVAL_MS;
			LeaveCriticalSection(&client->lock);
			if (bitmap_pending && !waiting_for_ack)
			{
				const uint64_t now = monotonic_milliseconds();
				if (last_send_started_at == 0 ||
				    now - last_send_started_at >= interval)
					timeout = 0;
				else
					timeout = WINPR_ASSERTING_INT_CAST(
					    DWORD, interval - (now - last_send_started_at));
			}
		}
		/* A blocked transport must wait for socket readiness, never spin on a due frame. */
		if (timeout == 0 && peer->IsWriteBlocked && peer->IsWriteBlocked(peer))
			timeout = 20;
		const DWORD status = WaitForMultipleObjects(count, handles, FALSE, timeout);
		if (status == WAIT_FAILED)
			break;
		if (WaitForSingleObject(client->shutdown_event, 0) == WAIT_OBJECT_0)
			break;
		/* Read key/button releases before spending time encoding the next frame. */
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
	server_reap_finished_peer_thread(server);
	EnterCriticalSection(&server->lock);
	const bool thread_busy = server->peer_thread != NULL;
	const bool active = server->active != NULL;
	const bool closing = server->active_closing;
	const bool busy = stop_requested || thread_busy || active || closing;
	if (busy)
	{
		LeaveCriticalSection(&server->lock);
		char diagnostic[160];
		(void)snprintf(diagnostic, sizeof(diagnostic),
		               "single-client 제한으로 새 RDP 연결을 listener에서 거부합니다 peer_thread=%u active=%u closing=%u stop=%u",
		               (unsigned)thread_busy, (unsigned)active, (unsigned)closing,
		               (unsigned)stop_requested);
		log_message("WARN", diagnostic);
		return FALSE;
	}
	peer->ContextExtra = server;
	server->peer_thread = CreateThread(NULL, 0, peer_thread, peer, 0, NULL);
	HANDLE thread = server->peer_thread;
	LeaveCriticalSection(&server->lock);
	if (!thread)
		return FALSE;
	return TRUE;
}

static void print_usage(const char* executable)
{
	(void)fprintf(stderr,
	              "Usage: %s [-listen host:port] [-cert file] [-key file] [-width n] [-height n] "
	              "[-bitrate n] [-control-port n] [-video-port n] [-direct-gfx] "
	              "[-swap-alt-command]\n",
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
		else if (strcmp(argv[index], "-direct-gfx") == 0)
			server.config.direct_gfx = true;
		else if (strcmp(argv[index], "-swap-alt-command") == 0)
			server.config.swap_alt_command = true;
		else
		{
			print_usage(argv[0]);
			return 2;
		}
	}
	if (server.config.width == 0 || server.config.height == 0 || server.config.bitrate == 0 ||
	    server.config.control_port == 0 || server.config.video_port == 0)
		return 2;
	server.next_keepalive_at = monotonic_milliseconds() + GATEWAY_KEEPALIVE_INTERVAL_MS;
	server.keepalive_dx = 1;
	if (!InitializeCriticalSectionAndSpinCount(&server.lock, 4000))
		return 1;
	const AgentTransportCallbacks transport_callbacks = {
		.event = on_transport_event,
		.clock = transport_clock,
		.context = &server,
	};
	if (!agent_transport_init(&server.transport, &transport_callbacks))
	{
		DeleteCriticalSection(&server.lock);
		return 1;
	}
	server.device_session = device_session_create(&(DeviceSessionConfig){
		.video_port = server.config.video_port,
		.video_source = {
			.request_idr = request_video_idr,
			.request_idr_context = &server,
		},
	});
	if (!server.device_session)
	{
		agent_transport_destroy(&server.transport);
		DeleteCriticalSection(&server.lock);
		return 1;
	}
	if (!WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi()) ||
	    !winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT))
	{
		device_session_destroy(server.device_session);
		agent_transport_destroy(&server.transport);
		DeleteCriticalSection(&server.lock);
		return 1;
	}

	freerdp_listener* listener = freerdp_listener_new();
	if (!listener)
	{
		device_session_destroy(server.device_session);
		agent_transport_destroy(&server.transport);
		DeleteCriticalSection(&server.lock);
		return 1;
	}
	server.control_listener = open_control_listener(server.config.bind_address, server.config.control_port);
	if (server.control_listener < 0)
	{
		freerdp_listener_free(listener);
		device_session_destroy(server.device_session);
		agent_transport_destroy(&server.transport);
		DeleteCriticalSection(&server.lock);
		return 1;
	}
	server.control_thread = CreateThread(NULL, 0, control_thread, &server, 0, NULL);
	if (!server.control_thread)
	{
		(void)close(server.control_listener);
		freerdp_listener_free(listener);
		device_session_destroy(server.device_session);
		agent_transport_destroy(&server.transport);
		DeleteCriticalSection(&server.lock);
		return 1;
	}
	listener->info = &server;
	listener->PeerAccepted = peer_accepted;
	WSADATA wsa = WINPR_C_ARRAY_INIT;
	const bool wsa_started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
	if (!wsa_started || !listener->Open(listener, server.config.bind_address, server.config.port))
	{
		stop_requested = 1;
		(void)shutdown(server.control_listener, SHUT_RDWR);
		(void)close(server.control_listener);
		server.control_listener = -1;
		agent_transport_shutdown(&server.transport);
		(void)WaitForSingleObject(server.control_thread, INFINITE);
		(void)CloseHandle(server.control_thread);
		freerdp_listener_free(listener);
		device_session_destroy(server.device_session);
		agent_transport_destroy(&server.transport);
		if (wsa_started)
			WSACleanup();
		DeleteCriticalSection(&server.lock);
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
	stop_requested = 1;
	listener->Close(listener);
	freerdp_listener_free(listener);
	if (!server_join_peer_thread(&server))
		return 1;
	if (server.control_listener >= 0)
	{
		(void)shutdown(server.control_listener, SHUT_RDWR);
		(void)close(server.control_listener);
		server.control_listener = -1;
	}
	agent_transport_shutdown(&server.transport);
	(void)WaitForSingleObject(server.control_thread, INFINITE);
	(void)CloseHandle(server.control_thread);
	agent_transport_destroy(&server.transport);
	device_session_destroy(server.device_session);
	WSACleanup();
	DeleteCriticalSection(&server.lock);
	return 0;
}
