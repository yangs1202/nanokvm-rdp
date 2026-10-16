#include "gateway_internal.h"
bool client_should_stop(Client* client)
{
	bool stopping = false;
	EnterCriticalSection(&client->lock);
	stopping = client->stopping;
	LeaveCriticalSection(&client->lock);
	return stopping;
}

void client_stop(Client* client)
{
	EnterCriticalSection(&client->lock);
	client->stopping = true;
	LeaveCriticalSection(&client->lock);
	if (client->output_wakeup)
		(void)SetEvent(client->output_wakeup);
	if (client->shutdown_event)
		(void)SetEvent(client->shutdown_event);
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
	gateway_log("INFO", "RDP desktop을 NanoKVM 원본 1920x1080으로 재협상");
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
	gateway_log("INFO", "RDP 협상 desktop %ux%u에 맞춰 gateway bitmap을 확대합니다",
	            width, height);
	return true;
}

static BOOL client_context_new(freerdp_peer* peer, rdpContext* context)
{
	Client* client = (Client*)context;
	Server* server = (Server*)peer->ContextExtra;
	if (!server || !InitializeCriticalSectionAndSpinCount(&client->lock, 4000))
		return FALSE;
	if (!InitializeCriticalSectionAndSpinCount(&client->output_lock, 4000))
	{
		DeleteCriticalSection(&client->lock);
		return FALSE;
	}
	client->output_lock_ready = true;
	client->server = server;
	client->peer = peer;
	client->shutdown_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	client->output_wakeup = CreateEvent(NULL, FALSE, FALSE, NULL);
	if (!client->shutdown_event || !client->output_wakeup)
		goto fail;
	client->need_idr = true;
	input_router_init(&client->input_router, server->config.width, server->config.height,
	                  server->config.swap_alt_command);
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
		gateway_log("WARN", "single-client 제한으로 새 RDP 연결을 거부합니다");
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
		restore_http_stream(client->server);
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
	if (client->output_thread)
	{
		(void)WaitForSingleObject(client->output_thread, 3000);
		(void)CloseHandle(client->output_thread);
	}
	if (client->output_wakeup)
		(void)CloseHandle(client->output_wakeup);
	if (client->gfx)
		rdpgfx_server_context_free(client->gfx);
	if (client->rfx)
		rfx_context_free(client->rfx);
	if (client->nsc)
		nsc_context_free(client->nsc);
	if (client->progressive)
		progressive_context_free(client->progressive);
	if (client->bitmap_stream)
		Stream_Free(client->bitmap_stream, TRUE);
	av_frame_free(&client->pending_decoded);
	av_frame_free(&client->pending_next_decoded);
	ffmpeg_converter_free(&client->converter);
	free(client->previous_bitmap);
	free(client->classic_encoded);
	if (client->vcm && client->vcm != INVALID_HANDLE_VALUE)
		WTSCloseServer(client->vcm);
	h264_parameter_sets_free(&client->parameter_sets);
	if (client->server && client->owns_active_client)
	{
		EnterCriticalSection(&client->server->lock);
		client->server->active_closing = false;
		LeaveCriticalSection(&client->server->lock);
	}
	if (client->output_lock_ready)
		DeleteCriticalSection(&client->output_lock);
	DeleteCriticalSection(&client->lock);
	gateway_log("INFO", "RDP client 연결 종료 및 USB HID release 완료");
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
		gateway_log("ERROR", "client가 RemoteFX/NSCodec을 지원하지 않아 raw bitmap으로 내려가지 않습니다");
		return false;
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
	if (!client->output_thread)
	{
		client->output_thread = CreateThread(NULL, 0, output_thread, client, 0, NULL);
		if (!client->output_thread)
			return false;
	}
	if (!server_set_stream_requested(client->server, true))
	{
		gateway_log("ERROR", "NanoKVM agent에 START_STREAM을 보낼 수 없습니다");
		return false;
	}
	if (client->output_wakeup)
		(void)SetEvent(client->output_wakeup);
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
		gateway_log("INFO", "RDP session activation 완료; RDPGFX dynamic channel open 대기 중");
	}
	else
	{
		if (!client_prepare_bitmap(client))
			return FALSE;
		gateway_log("INFO", "RDP session activation 완료; RTP/H.264 bitmap backend 시작");
	}
	return TRUE;
}

static bool client_process_dynamic_channels(Client* client)
{
	if (client->gfx_wait_started_at == 0 ||
	    !WTSVirtualChannelManagerIsChannelJoined(client->vcm, DRDYNVC_SVC_CHANNEL_NAME))
		return true;

	/* FreeRDP sends the drdynvc capability request only from ManagerOpen.
	 * Joining the static channel does not start that exchange, so state stays
	 * NONE and graphics Open() cannot succeed. */
	if (!client->drdynvc_open_attempted)
	{
		client->drdynvc_open_attempted = true;
		if (!WTSVirtualChannelManagerOpen(client->vcm))
		{
			gateway_log("ERROR", "drdynvc capability request 전송 실패");
			return false;
		}
		gateway_log("INFO", "drdynvc capability request 전송 state=%u",
		            (unsigned)WTSVirtualChannelManagerGetDrdynvcState(client->vcm));
	}

	/* Flush only when the queue is ready. A large frame write blocks this thread,
	 * and FreeRDP treats a client read that waits longer than 100 ms as fatal. */
	const HANDLE channel_event = WTSVirtualChannelManagerGetEventHandle(client->vcm);
	if (channel_event && WaitForSingleObject(channel_event, 0) == WAIT_OBJECT_0 &&
	    !WTSVirtualChannelManagerCheckFileDescriptor(client->vcm))
		return false;

	if (!client->gfx)
		return true;
	if (!client->gfx_opened &&
	    WTSVirtualChannelManagerGetDrdynvcState(client->vcm) == DRDYNVC_STATE_READY)
	{
		if (!client->gfx->Open || !client->gfx->Open(client->gfx))
		{
			gateway_log("ERROR", "RDPGFX dynamic channel open 실패");
			return false;
		}
		client->gfx_opened = true;
		client->gfx_opened_at = monotonic_milliseconds();
		gateway_log("INFO", "RDPGFX dynamic channel open 완료; client capability 대기 중");
	}
	if (client->gfx_opened)
	{
		HANDLE event = rdpgfx_server_get_event_handle(client->gfx);
		if (event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0)
		{
			const UINT error = rdpgfx_server_handle_messages(client->gfx);
			if (error != CHANNEL_RC_OK)
			{
				gateway_log("ERROR", "RDPGFX channel message 처리 실패");
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
	const uint64_t now = monotonic_milliseconds();
	const uint64_t started_at = client->gfx_opened ? client->gfx_opened_at : client->gfx_wait_started_at;
	if (now - started_at <= 15000)
		return true;
	gateway_log("ERROR",
	            "RDPGFX 준비 실패로 연결을 종료합니다 opened=%u state=%u waited_ms=%llu",
	            (unsigned)client->gfx_opened,
	            (unsigned)WTSVirtualChannelManagerGetDrdynvcState(client->vcm),
	            (unsigned long long)(now - client->gfx_wait_started_at));
	return false;
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
	    !freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, server->config.direct_gfx) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, server->config.direct_gfx) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_GfxProgressive,
	                               server->config.direct_gfx) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_GfxProgressiveV2,
	                               server->config.direct_gfx) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, TRUE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_NSCodec, TRUE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_DrawAllowDynamicColorFidelity, TRUE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_DrawAllowColorSubsampling, TRUE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_DrawAllowSkipAlpha, TRUE) ||
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
	    !freerdp_settings_set_uint32(settings, FreeRDP_MultifragMaxRequestSize, 0xFFFFFFU) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_OsMajorType, OSMAJORTYPE_WINDOWS) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_OsMinorType, OSMINORTYPE_WINDOWS_NT))
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

DWORD WINAPI peer_thread(LPVOID argument)
{
	freerdp_peer* peer = (freerdp_peer*)argument;
	Server* server = (Server*)peer->ContextExtra;
	if (!configure_peer(peer, server))
		goto out;

	Client* client = (Client*)peer->context;
	gateway_log("INFO", "TLS RDP client 연결 수락");
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
		if (count >= ARRAYSIZE(handles))
			break;
		handles[count++] = client->shutdown_event;
		/* Frame writes run on output_thread. This loop only reads input so a
		 * slow mobile socket cannot stall mouse events for the write duration. */
		const DWORD status = WaitForMultipleObjects(count, handles, FALSE, 50);
		if (status == WAIT_FAILED)
			break;
		if (WaitForSingleObject(client->shutdown_event, 0) == WAIT_OBJECT_0)
			break;
		if (!peer->CheckFileDescriptor(peer))
			break;
		if (client->direct_gfx_active && !client_process_dynamic_channels(client))
			break;
		if (peer->HasMoreToRead && peer->HasMoreToRead(peer) && !peer->CheckFileDescriptor(peer))
			break;
		if (client->direct_gfx_active && !client_check_gfx_timeout(client))
			break;
	}
	/* Stop output before Disconnect. IsWriteBlocked asserts if frontBio is
	 * already cleared, and that abort takes the whole gateway down. */
	client_stop(client);
	if (client->output_thread)
		(void)WaitForSingleObject(client->output_thread, 1000);
out:
	if (peer->Disconnect)
		peer->Disconnect(peer);
	freerdp_peer_context_free(peer);
	freerdp_peer_free(peer);
	return 0;
}

BOOL peer_accepted(freerdp_listener* listener, freerdp_peer* peer)
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
		gateway_log("WARN",
		            "single-client 제한으로 새 RDP 연결을 listener에서 거부합니다 peer_thread=%u active=%u closing=%u stop=%u",
		            (unsigned)thread_busy, (unsigned)active, (unsigned)closing,
		            (unsigned)stop_requested);
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
