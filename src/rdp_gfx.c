#include "gateway_internal.h"
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

UINT on_gfx_caps_advertise(RdpgfxServerContext* gfx,
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
		gateway_log("INFO", "RDPGFX client capability[%u]: version=0x%08x flags=0x%08x",
		            index, advertise->capsSets[index].version,
		            advertise->capsSets[index].flags);
	}
	if (!client_select_gfx_cap(advertise, &selected, &use_avc420))
	{
		gateway_log("ERROR", "client가 지원 가능한 RDPGFX capability를 광고하지 않았습니다");
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
	(void)server_set_stream_requested(client->server, true);
	if (use_avc420)
		gateway_log("INFO", "RDPGFX AVC420 capability 확인 및 surface 초기화 완료");
	else
		gateway_log("INFO", "RDPGFX Progressive capability 확인 및 surface 초기화 완료");
	return CHANNEL_RC_OK;
}

UINT on_gfx_frame_ack(RdpgfxServerContext* gfx,
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
	if (client->avc_ack_event)
		(void)SetEvent(client->avc_ack_event);
	gateway_log(elapsed >= 200U ? "WARN" : "INFO",
	            "RDP frame ACK id=%u decoded=%u queue_bytes=%u pending=%u matched=%u elapsed_ms=%llu",
	            acknowledge->frameId, acknowledge->totalFramesDecoded, acknowledge->queueDepth,
	            pending, (unsigned)matched, (unsigned long long)elapsed);
	if (trace.valid && frame_trace_sample(trace.rtp_timestamp))
	{
		gateway_log("INFO",
		            "FRAME_ACK ssrc=%u rtp_ts=%u frame_id=%u ack_ms=%llu send_to_ack_ms=%llu receive_to_ack_ms=%llu queue_bytes=%u",
		            trace.ssrc, trace.rtp_timestamp, trace.frame_id,
		            (unsigned long long)acknowledged_at, (unsigned long long)elapsed,
		            (unsigned long long)(acknowledged_at - trace.received_at), acknowledge->queueDepth);
	}
	if (client->output_wakeup) (void)SetEvent(client->output_wakeup);
	return CHANNEL_RC_OK;
}

static bool client_accept_avc_frame(Client* client, uint32_t bytes, bool idr)
{
	uint64_t wait_started_at = 0;
	for (;;)
	{
		bool request_idr = false;
		unsigned pending = 0;
		uint32_t inflight_bytes = 0;
		uint64_t now;
		FrameFlowAvcAdmission admission;
		EnterCriticalSection(&client->lock);
		now = monotonic_milliseconds();
		if (!client->gfx_ready || client->stopping)
		{
			LeaveCriticalSection(&client->lock);
			return false;
		}
		admission = frame_flow_avc_admit(&client->frame_flow, bytes, idr, now,
		                                 wait_started_at);
		if (admission == FRAME_FLOW_AVC_WAIT && wait_started_at == 0)
		{
			wait_started_at = now;
			client->stats.avc_waits++;
		}
		if (admission == FRAME_FLOW_AVC_RESYNC)
		{
			request_idr = frame_flow_avc_flow_rejected(&client->need_idr);
			client->stats.avc_skipped++;
			if (request_idr)
				client->stats.avc_resyncs++;
			pending = client->frame_flow.count;
			inflight_bytes = client->frame_flow.inflight_bytes;
		}
		LeaveCriticalSection(&client->lock);
		if (admission == FRAME_FLOW_AVC_SEND)
			return true;
		if (admission == FRAME_FLOW_AVC_RESYNC)
		{
			if (request_idr)
			{
				gateway_log("WARN",
				            "RDPGFX 흐름 제어로 frame을 건너뜀; 다음 IDR까지 전송을 멈춥니다 idr=%u bytes=%u pending=%u inflight_bytes=%u",
				            idr ? 1U : 0U, bytes, pending, inflight_bytes);
				(void)server_send_control(client->server, NANOKVM_CONTROL_IDR_REQUEST, NULL, 0);
			}
			return false;
		}
		if (client_should_stop(client))
			return false;
		const uint64_t elapsed = now - wait_started_at;
		const DWORD remaining = elapsed >= FRAME_FLOW_AVC_WAIT_MS
		                            ? 0U : (DWORD)(FRAME_FLOW_AVC_WAIT_MS - elapsed);
		if (!client->avc_ack_event ||
		    WaitForSingleObject(client->avc_ack_event, remaining) == WAIT_FAILED)
			return false;
	}
}

static bool send_avc420_frame(Client* client, const uint8_t* data, size_t length,
                              uint32_t ssrc, uint32_t rtp_timestamp, uint64_t received_at,
                              uint64_t receive_wall, bool idr)
{
	RECTANGLE_16 rect = { .left = 0, .top = 0, .right = client->server->config.width,
		.bottom = client->server->config.height };
	RDPGFX_H264_QUANT_QUALITY quality = { .qpVal = 0, .qualityVal = 100, .qp = 0, .r = 0, .p = 0 };
	RDPGFX_AVC420_BITMAP_STREAM avc = WINPR_C_ARRAY_INIT;
	RDPGFX_SURFACE_COMMAND command = WINPR_C_ARRAY_INIT;
	RDPGFX_START_FRAME_PDU start = WINPR_C_ARRAY_INIT;
	RDPGFX_END_FRAME_PDU end = WINPR_C_ARRAY_INIT;
	UINT error = CHANNEL_RC_OK;

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
	if (!client->gfx_ready || client->stopping)
	{
		LeaveCriticalSection(&client->lock);
		return false;
	}
	start.frameId = client->next_frame_id++;
	start.timestamp = 0;
	end.frameId = start.frameId;
	frame_trace_sent(&client->frame_trace, start.frameId, ssrc, rtp_timestamp, received_at, send_started);
	LeaveCriticalSection(&client->lock);
	if (!client->gfx || !client->gfx->SurfaceFrameCommand)
		error = ERROR_INVALID_HANDLE;
	else
		error = client->gfx->SurfaceFrameCommand(client->gfx, &command, &start, &end);
	const uint64_t send_done = monotonic_milliseconds();
	EnterCriticalSection(&client->lock);
	client->stats.last_rdp_send_ms = send_done - send_started;
	LeaveCriticalSection(&client->lock);
	if (frame_trace_sample(rtp_timestamp))
	{
		gateway_log("INFO",
		            "FRAME_SEND ssrc=%u rtp_ts=%u frame_id=%u receive_ms=%llu receive_wall_ms=%llu send_start_ms=%llu send_done_ms=%llu receive_to_send_ms=%llu submit_ms=%llu bytes=%zu ok=%u",
		            ssrc, rtp_timestamp, start.frameId, (unsigned long long)received_at,
		            (unsigned long long)receive_wall, (unsigned long long)send_started,
		            (unsigned long long)send_done, (unsigned long long)(send_started - received_at),
		            (unsigned long long)(send_done - send_started), length, error == CHANNEL_RC_OK);
	}
	if (error != CHANNEL_RC_OK)
	{
		EnterCriticalSection(&client->lock);
		frame_flow_avc_send_failed(&client->need_idr);
		client->stats.avc_skipped++;
		LeaveCriticalSection(&client->lock);
		gateway_log("ERROR", "RDPGFX AVC420 frame 전송 실패");
		return false;
	}
	EnterCriticalSection(&client->lock);
	if (idr)
		(void)frame_flow_sent_keyframe_sized(&client->frame_flow, start.frameId,
		                                     (uint32_t)length, send_done);
	else
		(void)frame_flow_sent_sized(&client->frame_flow, start.frameId, (uint32_t)length, send_done);
	frame_flow_avc_sent(&client->need_idr, idr);
	client->stats.bitmap_frames++;
	const uint32_t frame_count = client->stats.bitmap_frames;
	LeaveCriticalSection(&client->lock);
	if (frame_count == 1)
		gateway_log("INFO", "NanoKVM RTP/H.264 → RDPGFX AVC420 첫 frame 전송 완료");
	return true;
}

DWORD WINAPI video_thread(LPVOID argument)
{
	Client* client = (Client*)argument;
	DeviceSessionLease lease = { 0 };
	HANDLE yield_event = client->server->http_yield_event;
	if (yield_event)
		(void)SetEvent(yield_event);
	for (unsigned attempt = 0; attempt < 20 && !client_should_stop(client); attempt++)
	{
		if (device_session_acquire(client->server->device_session,
		                           DEVICE_SESSION_CONSUMER_DIRECT_GFX, &lease))
			break;
		Sleep(50);
	}
	if (yield_event)
		(void)ResetEvent(yield_event);
	if (!lease.session)
	{
		gateway_log("ERROR", "RDPGFX RTP/H.264 receiver를 시작할 수 없습니다");
		client_stop(client);
		return 0;
	}
	gateway_log("INFO", "RDPGFX RTP/H.264 passthrough 시작");
	gateway_log("INFO", "RTP receive buffer bytes=%d",
	            device_session_receive_buffer_bytes(&lease));
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
			gateway_log("ERROR", "RDPGFX RTP/H.264 frame 수신 실패");
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
		const uint32_t nal_types = access_unit.nal_types;
		const bool idr = (nal_types & (1U << 5)) != 0;
		const bool p_frame = (nal_types & (1U << 1)) != 0;
		h264_parameter_sets_update(&client->parameter_sets, data, length, nal_types);
		bool should_send;
		EnterCriticalSection(&client->lock);
		client->stats.rtp_nals++;
		client->stats.rtp_access_units++;
		if (idr)
			client->stats.rtp_idr_units++;
		if (p_frame)
			client->stats.rtp_p_units++;
		client->last_rtp_received_at = received_at;
		if (access_unit.packet_loss)
			frame_flow_avc_send_failed(&client->need_idr);
		should_send = !access_unit.packet_loss &&
		              frame_flow_avc_should_send(client->need_idr, idr, p_frame) &&
		              client->gfx_ready && !client->stopping;
		if (!should_send && client->gfx_ready && !client->stopping &&
		    (access_unit.packet_loss || client->need_idr))
			client->stats.avc_skipped++;
		LeaveCriticalSection(&client->lock);
		if (access_unit.packet_loss)
			gateway_log("WARN", "RDPGFX RTP frame loss 감지; NanoKVM agent에 IDR 재동기화를 요청합니다");

		if (should_send)
		{
			const uint8_t* payload = data;
			size_t payload_length = length;
			if (idr)
			{
				if (!h264_parameter_sets_prepare_idr(&client->parameter_sets, data, length,
				                                     nal_types, &payload, &payload_length))
				{
					EnterCriticalSection(&client->lock);
					frame_flow_avc_send_failed(&client->need_idr);
					client->stats.avc_skipped++;
					LeaveCriticalSection(&client->lock);
					client_stop(client);
					video_source_release_access_unit(&access_unit);
					break;
				}
			}
			if (!client_accept_avc_frame(client, (uint32_t)payload_length, idr))
				goto release_access_unit;
			if (!send_avc420_frame(client, payload, payload_length,
			                       access_unit.ssrc, access_unit.timestamp,
			                       received_at, receive_wall, idr))
				client_stop(client);
		}
	release_access_unit:
		video_source_release_access_unit(&access_unit);
	}
	device_session_release(&lease);
	return 0;
}
