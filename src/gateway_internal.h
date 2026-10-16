#ifndef GATEWAY_INTERNAL_H
#define GATEWAY_INTERNAL_H

#include "bitmap_diff.h"
#include "agent_transport.h"
#include "device_session.h"
#include "ffmpeg_decoder.h"
#include "frame_flow.h"
#include "frame_trace.h"
#include "gateway_config.h"
#include "h264.h"
#include "hid.h"
#include "http_api.h"
#include "input_router.h"
#include "protocol.h"
#include "video_source.h"

#include <freerdp/channels/channels.h>
#include <freerdp/channels/drdynvc.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/codec/color.h>
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
#include <freerdp/constants.h>

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
#define MAX_EVENT_HANDLES 32U
#define BITMAP_FRAME_INTERVAL_MS 10U
#define GATEWAY_KEEPALIVE_INTERVAL_MS 10000U
#define CLASSIC_TILE_WIDTH 64U
#define CLASSIC_TILE_HEIGHT 64U
#define CLASSIC_TILE_MAX_ENCODED (CLASSIC_TILE_WIDTH * CLASSIC_TILE_HEIGHT * 2U)
#define CLASSIC_BITMAP_BATCH 1U
#define CLASSIC_MAX_UPDATE_SIZE (32U * 1024U)
#define STATS_LOG_INTERVAL_MS 5000U
#define PEER_SHUTDOWN_JOIN_TIMEOUT_MS 5000U
#define CLASSIC_SLICE_BUDGET_MS 4U
#define WHEEL_ROTATION_MASK 0x01FFU

/* Counters reported by the periodic STATS line. Guarded by Client.lock. */
typedef struct
{
	uint32_t bitmap_frames;
	uint32_t decoded_frames;
	uint32_t converted_frames;
	uint32_t bitmap_queued_frames;
	uint32_t bitmap_queue_drops;
	uint32_t bitmap_stale_drops;
	uint32_t bitmap_flushes;
	uint32_t bitmap_empty_flushes;
	uint64_t classic_tiles_sent;
	uint64_t classic_bytes_sent;
	uint64_t pointer_position_updates;
	uint32_t rtp_nals;
	uint32_t rtp_access_units;
	uint32_t rtp_idr_units;
	uint32_t rtp_p_units;
	uint32_t avc_skipped;
	uint32_t avc_resyncs;
	uint32_t avc_waits;
	uint64_t last_decode_latency_ms;
	uint64_t last_rdp_send_ms;
} ClientStats;

typedef struct Server Server;
typedef struct Client Client;

struct Server
{
	GatewayConfig config;
	CRITICAL_SECTION lock;
	int control_listener;
	HANDLE control_thread;
	HANDLE peer_thread;
	AgentTransport transport;
	DeviceSession* device_session;
	HttpApi* http_api;
	HANDLE http_video_thread;
	HANDLE http_yield_event;
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
	HANDLE output_thread;
	HANDLE output_wakeup;
	CRITICAL_SECTION output_lock;
	bool output_lock_ready;
	RFX_CONTEXT* rfx;
	NSC_CONTEXT* nsc;
	PROGRESSIVE_CONTEXT* progressive;
	wStream* bitmap_stream;
	CRITICAL_SECTION lock;
	AVFrame* pending_decoded;
	AVFrame* pending_next_decoded;
	FfmpegConverter converter;
	uint8_t* previous_bitmap;
	size_t previous_bitmap_length;
	uint8_t* classic_encoded;
	bool previous_bitmap_valid;
	HANDLE shutdown_event;
	HANDLE avc_ack_event;
	bool stopping;
	bool owns_active_client;
	bool direct_gfx_active;
	bool bitmap_fallback_active;
	bool gfx_ready;
	bool gfx_opened;
	bool drdynvc_open_attempted;
	FrameFlow frame_flow;
	FrameTrace frame_trace;
	bool need_idr;
	uint64_t gfx_wait_started_at;
	uint64_t gfx_opened_at;
	uint32_t next_frame_id;
	H264ParameterSets parameter_sets;
	bool bitmap_uses_rfx;
	bool gfx_uses_progressive;
	uint16_t render_width;
	uint16_t render_height;
	uint64_t bitmap_last_send_started_at;
	uint16_t classic_resume_left;
	uint16_t classic_resume_top;
	bool classic_frame_open;
	bool keyboard_input_logged;
	bool pointer_input_logged;
	bool pointer_position_logged;
	bool wheel_input_logged;
	InputRouter input_router;
	ClientStats stats;
	uint64_t last_rtp_received_at;
};

/* src/main.c state */
extern volatile sig_atomic_t stop_requested;

/* src/gateway_log.c */
uint64_t monotonic_milliseconds(void);
void gateway_log(const char* level, const char* format, ...)
	__attribute__((format(printf, 2, 3)));

/* src/gateway_server.c */
bool server_send_control(Server* server, uint8_t type, const void* payload, uint16_t length);
bool server_set_stream_requested(Server* server, bool requested);
void restore_http_stream(Server* server);
bool request_video_idr(void* context, uint64_t video_source_epoch);
bool server_join_peer_thread(Server* server);
void server_reap_finished_peer_thread(Server* server);
DWORD WINAPI control_thread(LPVOID argument);
int open_control_listener(const char* host, uint16_t port);
uint64_t transport_clock(void* context);
void on_transport_event(void* context, AgentTransportEvent event, uint64_t epoch);
void server_heartbeat(Server* server);
DWORD WINAPI http_video_thread(LPVOID argument);
bool http_send_control(void* context, uint8_t type, const void* payload, uint16_t length);
bool http_set_stream(void* context, bool requested);

/* src/rdp_gfx.c */
UINT on_gfx_caps_advertise(RdpgfxServerContext* gfx,
	const RDPGFX_CAPS_ADVERTISE_PDU* advertise);
UINT on_gfx_frame_ack(RdpgfxServerContext* gfx,
	const RDPGFX_FRAME_ACKNOWLEDGE_PDU* acknowledge);
DWORD WINAPI video_thread(LPVOID argument);

/* src/rdp_bitmap.c */
bool bitmap_stream_rfx_supported(const rdpSettings* settings);
bool bitmap_stream_nsc_supported(const rdpSettings* settings);
void client_output_unlock(Client* client);
DWORD WINAPI output_thread(LPVOID argument);
DWORD WINAPI bitmap_video_thread(LPVOID argument);

/* src/rdp_input.c */
BOOL on_keyboard(rdpInput* input, UINT16 flags, UINT8 code);
BOOL on_unicode_keyboard(rdpInput* input, UINT16 flags, UINT16 code);
BOOL client_release_all_inputs(Client* client, const char* reason);
BOOL on_synchronize(rdpInput* input, UINT32 toggle_states);
BOOL on_mouse(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y);
BOOL on_extended_mouse(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y);
BOOL on_relative_mouse(rdpInput* input, UINT16 flags, INT16 x_delta, INT16 y_delta);

/* src/rdp_peer.c */
bool client_should_stop(Client* client);
void client_stop(Client* client);
DWORD WINAPI peer_thread(LPVOID argument);
BOOL peer_accepted(freerdp_listener* listener, freerdp_peer* peer);

#endif
