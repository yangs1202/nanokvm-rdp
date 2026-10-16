#include "gateway_config.h"
#include "gateway_internal.h"

volatile sig_atomic_t stop_requested = 0;

static void on_signal(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

/* Resources that are not represented by a handle or pointer in Server. */
typedef struct
{
	bool lock_ready;
	bool transport_ready;
	bool wsa_started;
	freerdp_listener* listener;
} GatewayRuntime;

static bool start_http_api(Server* server)
{
	const HttpApiConfig http_config = {
		.bind_address = server->config.bind_address,
		.port = server->config.http_port,
		.width = server->config.width,
		.height = server->config.height,
		.jpeg_quality = server->config.jpeg_quality,
		.swap_alt_command = server->config.swap_alt_command,
		.context = server,
		.send_control = http_send_control,
		.set_stream = http_set_stream,
	};
	server->http_api = http_api_create(&http_config);
	if (!server->http_api || !http_api_start(server->http_api))
		return false;
	server->http_video_thread = CreateThread(NULL, 0, http_video_thread, server, 0, NULL);
	if (!server->http_video_thread)
		return false;
	(void)fprintf(stderr, "%s: INFO: HTTP snapshot API listening on %s:%u\n", TAG,
	              server->config.bind_address, server->config.http_port);
	return true;
}

static bool gateway_start(Server* server, GatewayRuntime* runtime)
{
	server->control_listener = -1;
	server->next_keepalive_at = monotonic_milliseconds() + GATEWAY_KEEPALIVE_INTERVAL_MS;
	server->keepalive_dx = 1;
	if (!InitializeCriticalSectionAndSpinCount(&server->lock, 4000))
		return false;
	runtime->lock_ready = true;
	server->http_yield_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!server->http_yield_event)
		return false;
	const AgentTransportCallbacks transport_callbacks = {
		.event = on_transport_event,
		.clock = transport_clock,
		.context = server,
	};
	if (!agent_transport_init(&server->transport, &transport_callbacks))
		return false;
	runtime->transport_ready = true;
	server->device_session = device_session_create(&(DeviceSessionConfig){
		.video_port = server->config.video_port,
		.video_source = {
			.request_idr = request_video_idr,
			.request_idr_context = server,
		},
	});
	if (!server->device_session)
		return false;
	if (!WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi()) ||
	    !winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT))
		return false;
	if (!start_http_api(server))
		return false;

	runtime->listener = freerdp_listener_new();
	if (!runtime->listener)
		return false;
	server->control_listener =
	    open_control_listener(server->config.bind_address, server->config.control_port);
	if (server->control_listener < 0)
		return false;
	server->control_thread = CreateThread(NULL, 0, control_thread, server, 0, NULL);
	if (!server->control_thread)
		return false;
	runtime->listener->info = server;
	runtime->listener->PeerAccepted = peer_accepted;
	WSADATA wsa = WINPR_C_ARRAY_INIT;
	runtime->wsa_started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
	return runtime->wsa_started &&
	       runtime->listener->Open(runtime->listener, server->config.bind_address,
	                               server->config.port);
}

/* Reverse of gateway_start; safe after a partial start. Consumers stop before
 * the shared state they use: HTTP and RDP threads before the device session
 * and yield event, the control thread before the transport. Returns false when
 * the peer thread could not be joined; its state is then deliberately leaked. */
static bool gateway_stop(Server* server, GatewayRuntime* runtime)
{
	stop_requested = 1;
	http_api_stop(server->http_api);
	/* The reader observes stop_requested within one RTP receive timeout.
	 * It uses http_api, the device session and the yield event, so it must
	 * be fully joined before any of them is destroyed. */
	if (server->http_video_thread)
	{
		(void)WaitForSingleObject(server->http_video_thread, INFINITE);
		(void)CloseHandle(server->http_video_thread);
		server->http_video_thread = NULL;
	}
	http_api_destroy(server->http_api);
	server->http_api = NULL;
	if (runtime->listener)
	{
		runtime->listener->Close(runtime->listener);
		freerdp_listener_free(runtime->listener);
		runtime->listener = NULL;
	}
	if (runtime->lock_ready && !server_join_peer_thread(server))
		return false;
	if (server->control_listener >= 0)
	{
		(void)shutdown(server->control_listener, SHUT_RDWR);
		(void)close(server->control_listener);
		server->control_listener = -1;
	}
	if (runtime->transport_ready)
		agent_transport_shutdown(&server->transport);
	if (server->control_thread)
	{
		(void)WaitForSingleObject(server->control_thread, INFINITE);
		(void)CloseHandle(server->control_thread);
		server->control_thread = NULL;
	}
	if (runtime->transport_ready)
		agent_transport_destroy(&server->transport);
	device_session_destroy(server->device_session);
	server->device_session = NULL;
	if (server->http_yield_event)
		(void)CloseHandle(server->http_yield_event);
	server->http_yield_event = NULL;
	if (runtime->wsa_started)
		WSACleanup();
	if (runtime->lock_ready)
		DeleteCriticalSection(&server->lock);
	return true;
}

int main(int argc, char* argv[])
{
	Server server = WINPR_C_ARRAY_INIT;
	char error[160] = { 0 };
	gateway_config_defaults(&server.config);
	if (!gateway_config_parse(&server.config, argc, argv, error, sizeof(error)))
	{
		(void)fprintf(stderr, "%s: ERROR: %s\n", TAG, error);
		gateway_config_print_usage(stderr, argv[0]);
		return 2;
	}

	GatewayRuntime runtime = { 0 };
	if (!gateway_start(&server, &runtime))
	{
		(void)gateway_stop(&server, &runtime);
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
		const DWORD count =
		    runtime.listener->GetEventHandles(runtime.listener, handles, ARRAYSIZE(handles));
		if (count == 0 || WaitForMultipleObjects(count, handles, FALSE, 200) == WAIT_FAILED)
			break;
		if (!runtime.listener->CheckFileDescriptor(runtime.listener))
			break;
	}
	return gateway_stop(&server, &runtime) ? 0 : 1;
}
