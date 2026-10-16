#include "gateway_internal.h"
bool server_send_control(Server* server, uint8_t type, const void* payload, uint16_t length)
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
	if (idle && server->http_api && !http_api_input_idle(server->http_api))
		idle = false;
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

bool server_set_stream_requested(Server* server, bool requested)
{
	const bool sent = agent_transport_set_stream_requested(&server->transport, requested);
	gateway_log(sent ? "INFO" : "ERROR", requested ? "NanoKVM agent에 START_STREAM 전송"
	                                                : "NanoKVM agent에 STOP_STREAM 전송");
	return sent;
}

/* RDP teardown always clears the agent stream. HTTP snapshots have no RDP
 * client, so restore the request the HTTP API made at startup. */
void restore_http_stream(Server* server)
{
	if (server && server->http_api)
		(void)server_set_stream_requested(server, true);
}

bool request_video_idr(void* context, uint64_t video_source_epoch)
{
	/* This epoch identifies the successful RTP open that produced the access
	 * unit. It is intentionally independent of AgentTransport's control
	 * connection epoch; server_send_control preserves the existing current-
	 * transport-epoch validation when it sends IDR_REQUEST. */
	(void)video_source_epoch;
	return server_send_control((Server*)context, NANOKVM_CONTROL_IDR_REQUEST, NULL, 0);
}

/* The peer thread exclusively performs FreeRDP teardown. Keep a running handle
 * until shutdown join; completed handles are reaped by listener admission so a
 * new peer can be accepted without outliving Server-owned state. */
bool server_join_peer_thread(Server* server)
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
		gateway_log("ERROR", "RDP peer thread가 종료 시간 제한 내에 정리되지 않았습니다");
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
void server_reap_finished_peer_thread(Server* server)
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
		gateway_log("INFO", "종료된 RDP peer thread handle을 재사용 가능 상태로 회수했습니다");
	}
}

DWORD WINAPI control_thread(LPVOID argument)
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
		gateway_log("INFO", "NanoKVM agent control 연결 수락");
		agent_transport_run(&server->transport, fd, epoch);
	}
	return 0;
}

int open_control_listener(const char* host, uint16_t port)
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

uint64_t transport_clock(void* context)
{
	(void)context;
	return monotonic_milliseconds();
}

void on_transport_event(void* context, AgentTransportEvent event, uint64_t epoch)
{
	Server* server = (Server*)context;
	if (event == AGENT_TRANSPORT_EVENT_DISCONNECTED)
	{
		gateway_log("WARN", "NanoKVM agent control 연결 종료");
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
		gateway_log("WARN", "NanoKVM agent heartbeat timeout; active RDP client 연결을 종료합니다");
	else
		gateway_log("WARN", "NanoKVM agent heartbeat timeout; control 연결을 종료합니다");
}

void server_heartbeat(Server* server)
{
	const uint64_t now = monotonic_milliseconds();
	agent_transport_heartbeat(&server->transport, now);
	server_keepalive(server, now);
	if (now - server->last_stats_log_at < STATS_LOG_INTERVAL_MS)
		return;
	server->last_stats_log_at = now;
	struct rusage usage = { 0 };
	if (getrusage(RUSAGE_SELF, &usage) != 0)
		return;
	AgentTransportStats agent_stats = { 0 };
	agent_transport_get_stats(&server->transport, &agent_stats);
	/* Snapshot under the client lock so one STATS line is self-consistent. */
	ClientStats stats = { 0 };
	unsigned interval_ms = 0;
	bool bitmap_pending = false;
	EnterCriticalSection(&server->lock);
	Client* active = server->active;
	if (active)
	{
		EnterCriticalSection(&active->lock);
		stats = active->stats;
		interval_ms = frame_flow_interval(&active->frame_flow);
		bitmap_pending = active->pending_decoded != NULL || active->pending_next_decoded != NULL;
		LeaveCriticalSection(&active->lock);
	}
	LeaveCriticalSection(&server->lock);
	gateway_log("INFO",
	            "STATS agent packets=%u dropped=%u frames=%u dropped_frames=%u rtp_nals=%u au=%u idr=%u p=%u decoded=%u converted=%u interval_ms=%u queued=%u queue_drop=%u stale_drop=%u flush=%u empty_flush=%u rdp_frames=%u classic_tiles=%llu classic_bytes=%llu pointer_updates=%llu queue=%u gateway_rss=%ld decode_ms=%llu rdp_send_ms=%llu avc_skipped=%u avc_resyncs=%u",
	            agent_stats.sent_packets, agent_stats.dropped_packets,
	            agent_stats.capture_frames, agent_stats.dropped_frames,
	            stats.rtp_nals, stats.rtp_access_units, stats.rtp_idr_units, stats.rtp_p_units,
	            stats.decoded_frames, stats.converted_frames, interval_ms,
	            stats.bitmap_queued_frames, stats.bitmap_queue_drops, stats.bitmap_stale_drops,
	            stats.bitmap_flushes, stats.bitmap_empty_flushes, stats.bitmap_frames,
	            (unsigned long long)stats.classic_tiles_sent,
	            (unsigned long long)stats.classic_bytes_sent,
	            (unsigned long long)stats.pointer_position_updates, bitmap_pending ? 1U : 0U,
	            usage.ru_maxrss, (unsigned long long)stats.last_decode_latency_ms,
	            (unsigned long long)stats.last_rdp_send_ms,
	            stats.avc_skipped, stats.avc_resyncs);
}

static bool on_http_decoded_frame(void* context, const AVFrame* frame)
{
	Server* server = context;
	if (stop_requested || !server->http_api)
		return false;
	/* A JPEG encode failure is not a broken H.264 session. Returning false
	 * here makes the decoder discard every following IDR. */
	(void)http_api_publish_frame(server->http_api, frame);
	return true;
}

/* HTTP snapshots do not require an RDP client. The device session still has
 * one reader, so this thread yields while an RDP client owns the RTP socket. */
DWORD WINAPI http_video_thread(LPVOID argument)
{
	Server* server = (Server*)argument;
	while (!stop_requested)
	{
		/* Keep the socket available until the RDP reader finishes acquiring
		 * its lease. Reacquiring while yield is signaled starves that reader. */
		if (server->http_yield_event &&
		    WaitForSingleObject(server->http_yield_event, 0) == WAIT_OBJECT_0)
		{
			Sleep(50);
			continue;
		}
		DeviceSessionLease lease = { 0 };
		FfmpegDecoder decoder = { 0 };
		if (!device_session_acquire(server->device_session, DEVICE_SESSION_CONSUMER_HTTP,
		                            &lease) ||
		    !ffmpeg_decoder_start_raw(&decoder, on_http_decoded_frame, server))
		{
			ffmpeg_decoder_stop(&decoder);
			device_session_release(&lease);
			Sleep(200);
			continue;
		}
		bool yielded = false;
		gateway_log("INFO", "HTTP JPEG snapshot decoder 시작");
		H264ParameterSets parameter_sets = { 0 };
		H264Buffer fallback = { 0 };
		bool need_idr = true;
		while (!stop_requested && !yielded)
		{
			if (server->http_yield_event &&
			    WaitForSingleObject(server->http_yield_event, 0) == WAIT_OBJECT_0)
			{
				yielded = true;
				break;
			}
			VideoSourceAccessUnit access_unit = { 0 };
			if (!device_session_read(&lease, &access_unit))
			{
				const int error = errno;
				if (stop_requested)
					break;
				if (error == EAGAIN || error == EWOULDBLOCK)
				{
					if (server->http_yield_event &&
					    WaitForSingleObject(server->http_yield_event, 0) == WAIT_OBJECT_0)
					{
						yielded = true;
						break;
					}
					continue;
				}
				gateway_log("ERROR", "HTTP RTP/H.264 frame 수신 실패");
				break;
			}
			uint8_t* data = access_unit.data;
			const size_t length = access_unit.length;
			if (!data || length == 0)
			{
				video_source_release_access_unit(&access_unit);
				continue;
			}
			const uint32_t nal_types = access_unit.nal_types;
			const bool idr = (nal_types & (1U << 5)) != 0;
			h264_parameter_sets_update(&parameter_sets, data, length, nal_types);
			if (access_unit.packet_loss)
				need_idr = true;
			if (need_idr && !idr)
			{
				video_source_release_access_unit(&access_unit);
				continue;
			}
			const uint8_t* payload = data;
			size_t payload_length = length;
			if (idr)
			{
				if (!h264_parameter_sets_prepare_idr(&parameter_sets, data, length, nal_types,
				                                     &payload, &payload_length))
				{
					video_source_release_access_unit(&access_unit);
					break;
				}
			}
			const uint8_t* push_data = payload;
			size_t push_length = payload_length;
			if (!h264_ensure_annexb(&fallback, payload, payload_length, &push_data,
			                        &push_length))
			{
				video_source_release_access_unit(&access_unit);
				break;
			}
			const bool pushed = ffmpeg_decoder_push(&decoder, push_data, push_length);
			const uint32_t pushed_nal_types = h264_nal_type_mask(push_data, push_length);
			video_source_release_access_unit(&access_unit);
			if (!pushed && !stop_requested)
			{
				need_idr = true;
				gateway_log("WARN", "HTTP decoder skip bytes=%zu sps=%u pps=%u idr=%u sei=%u",
				            push_length,
				            (unsigned)((pushed_nal_types & (1U << 7)) != 0),
				            (unsigned)((pushed_nal_types & (1U << 8)) != 0),
				            (unsigned)((pushed_nal_types & (1U << 5)) != 0),
				            (unsigned)((pushed_nal_types & (1U << 6)) != 0));
				continue;
			}
			if (idr)
				need_idr = false;
		}
		h264_parameter_sets_free(&parameter_sets);
		h264_buffer_free(&fallback);
		ffmpeg_decoder_stop(&decoder);
		device_session_release(&lease);
		if (!stop_requested && !yielded)
			Sleep(200);
	}
	return 0;
}

bool http_send_control(void* context, uint8_t type, const void* payload, uint16_t length)
{
	return server_send_control(context, type, payload, length);
}

bool http_set_stream(void* context, bool requested)
{
	return server_set_stream_requested(context, requested);
}
