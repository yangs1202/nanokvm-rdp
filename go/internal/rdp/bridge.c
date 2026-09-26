#include "bridge.h"

#include <freerdp/freerdp.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/input.h>
#include <freerdp/listener.h>
#include <freerdp/peer.h>
#include <freerdp/settings.h>
#include <freerdp/update.h>
#include <winpr/ssl.h>
#include <winpr/synch.h>
#include <winpr/thread.h>
#include <winpr/wtsapi.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct
{
	rdpContext context;
	NanokvmRdpBridge* bridge;
	freerdp_peer* peer;
	atomic_bool stopping;
} NanokvmRdpClient;

struct NanokvmRdpBridge
{
	NanokvmRdpConfig config;
	NanokvmRdpInputCallback callback;
	void* callback_context;
	pthread_mutex_t lock;
	uint8_t* bitmap;
	size_t bitmap_length;
	uint16_t width;
	uint16_t height;
	uint8_t* h264;
	size_t h264_length;
	atomic_bool running;
	freerdp_listener* listener;
	pthread_t thread;
	bool thread_started;
};

static BOOL on_keyboard(rdpInput* input, UINT16 flags, UINT8 code)
{
	NanokvmRdpClient* client = (NanokvmRdpClient*)input->context;
	NanokvmRdpInput event = { .kind = 1, .flags = flags, .code = code };
	if (client->bridge->callback)
		client->bridge->callback(client->bridge->callback_context, &event);
	return TRUE;
}

static BOOL on_mouse(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
{
	NanokvmRdpClient* client = (NanokvmRdpClient*)input->context;
	NanokvmRdpInput event = { .kind = 3, .flags = flags, .x = (int16_t)x, .y = (int16_t)y };
	if (client->bridge->callback)
		client->bridge->callback(client->bridge->callback_context, &event);
	return TRUE;
}

static BOOL client_context_new(freerdp_peer* peer, rdpContext* context)
{
	NanokvmRdpClient* client = (NanokvmRdpClient*)context;
	client->bridge = peer->ContextExtra;
	client->peer = peer;
	atomic_store(&client->stopping, false);
	return TRUE;
}

static void client_context_free(freerdp_peer* peer, rdpContext* context)
{
	(void)peer;
	NanokvmRdpClient* client = (NanokvmRdpClient*)context;
	atomic_store(&client->stopping, true);
}

static bool send_bitmap(NanokvmRdpClient* client)
{
	NanokvmRdpBridge* bridge = client->bridge;
	pthread_mutex_lock(&bridge->lock);
	const size_t length = bridge->bitmap_length;
	const uint16_t width = bridge->width;
	const uint16_t height = bridge->height;
	uint8_t* copy = NULL;
	if (length > 0)
	{
		copy = malloc(length);
		if (copy)
			memcpy(copy, bridge->bitmap, length);
	}
	pthread_mutex_unlock(&bridge->lock);
	if (!copy)
		return true;
	BITMAP_DATA rectangle = { 0 };
	BITMAP_UPDATE update = { .number = 1, .rectangles = &rectangle, .skipCompression = TRUE };
	rectangle.destLeft = 0;
	rectangle.destTop = 0;
	rectangle.destRight = width > 0 ? width - 1 : 0;
	rectangle.destBottom = height > 0 ? height - 1 : 0;
	rectangle.width = width;
	rectangle.height = height;
	rectangle.bitsPerPixel = 32;
	rectangle.bitmapLength = (UINT16)(length > UINT16_MAX ? UINT16_MAX : length);
	rectangle.bitmapDataStream = copy;
	rectangle.compressed = FALSE;
	const BOOL sent = client->context.update && client->context.update->BitmapUpdate &&
	                  client->context.update->BitmapUpdate(&client->context, &update);
	free(copy);
	return sent == TRUE;
}

static BOOL peer_post_connect(freerdp_peer* peer)
{
	(void)peer;
	return TRUE;
}

static bool configure_peer(freerdp_peer* peer, NanokvmRdpBridge* bridge)
{
	peer->ContextSize = sizeof(NanokvmRdpClient);
	peer->ContextNew = client_context_new;
	peer->ContextFree = client_context_free;
	peer->ContextExtra = bridge;
	if (!freerdp_peer_context_new(peer))
		return false;
	rdpSettings* settings = peer->context->settings;
	rdpPrivateKey* key = freerdp_key_new_from_file(bridge->config.private_key);
	rdpCertificate* certificate = freerdp_certificate_new_from_file(bridge->config.certificate);
	if (!key || !certificate)
		return false;
	if (!freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key, 1) ||
	    !freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, certificate, 1) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, bridge->config.width) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, bridge->config.height) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32))
		return false;
	peer->PostConnect = peer_post_connect;
	peer->context->input->KeyboardEvent = on_keyboard;
	peer->context->input->MouseEvent = on_mouse;
	return peer->Initialize(peer) == TRUE;
}

static DWORD WINAPI peer_thread(LPVOID argument)
{
	freerdp_peer* peer = argument;
	NanokvmRdpBridge* bridge = peer->ContextExtra;
	if (!configure_peer(peer, bridge))
		goto out;
	NanokvmRdpClient* client = (NanokvmRdpClient*)peer->context;
	while (atomic_load(&bridge->running) && !atomic_load(&client->stopping))
	{
		HANDLE handles[32] = { 0 };
		DWORD count = peer->GetEventHandles(peer, handles, 32);
		if (count == 0)
			break;
		if (WaitForMultipleObjects(count, handles, FALSE, 30) == WAIT_FAILED)
			break;
		if (!peer->CheckFileDescriptor(peer))
			break;
		if (!send_bitmap(client))
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
	peer->ContextExtra = listener->info;
	HANDLE thread = CreateThread(NULL, 0, peer_thread, peer, 0, NULL);
	if (!thread)
		return FALSE;
	(void)CloseHandle(thread);
	return TRUE;
}

static void* listener_loop(void* argument)
{
	NanokvmRdpBridge* bridge = argument;
	while (atomic_load(&bridge->running))
	{
		HANDLE handles[8] = { 0 };
		DWORD count = bridge->listener->GetEventHandles(bridge->listener, handles, 8);
		if (count == 0 || WaitForMultipleObjects(count, handles, FALSE, 200) == WAIT_FAILED)
			break;
		if (!bridge->listener->CheckFileDescriptor(bridge->listener))
			break;
	}
	return NULL;
}

NanokvmRdpBridge* nanokvm_rdp_start(const NanokvmRdpConfig* config, NanokvmRdpInputCallback callback,
                                    void* context)
{
	if (!config || !config->bind_address || config->port == 0)
		return NULL;
	NanokvmRdpBridge* bridge = calloc(1, sizeof(*bridge));
	if (!bridge)
		return NULL;
	bridge->config = *config;
	bridge->callback = callback;
	bridge->callback_context = context;
	if (pthread_mutex_init(&bridge->lock, NULL) != 0)
	{
		free(bridge);
		return NULL;
	}
	atomic_store(&bridge->running, true);
	if (!WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi()) ||
	    !winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT))
		goto fail;
	bridge->listener = freerdp_listener_new();
	if (!bridge->listener)
		goto fail;
	bridge->listener->info = bridge;
	bridge->listener->PeerAccepted = peer_accepted;
	if (!bridge->listener->Open(bridge->listener, bridge->config.bind_address, bridge->config.port))
		goto fail;
	if (pthread_create(&bridge->thread, NULL, listener_loop, bridge) != 0)
		goto fail;
	bridge->thread_started = true;
	return bridge;
fail:
	nanokvm_rdp_stop(bridge);
	return NULL;
}

static bool copy_latest(uint8_t** destination, size_t* destination_length, const uint8_t* source,
                        size_t length)
{
	uint8_t* next = realloc(*destination, length);
	if (!next && length != 0)
		return false;
	memcpy(next, source, length);
	*destination = next;
	*destination_length = length;
	return true;
}

bool nanokvm_rdp_submit_bgra(NanokvmRdpBridge* bridge, const uint8_t* bgra, size_t length,
                             uint16_t width, uint16_t height)
{
	if (!bridge || !bgra || width == 0 || height == 0 ||
	    length != (size_t)width * height * 4U)
		return false;
	pthread_mutex_lock(&bridge->lock);
	const bool ok = copy_latest(&bridge->bitmap, &bridge->bitmap_length, bgra, length);
	if (ok)
	{
		bridge->width = width;
		bridge->height = height;
	}
	pthread_mutex_unlock(&bridge->lock);
	return ok;
}

bool nanokvm_rdp_submit_h264(NanokvmRdpBridge* bridge, const uint8_t* data, size_t length)
{
	if (!bridge || !data || length == 0)
		return false;
	pthread_mutex_lock(&bridge->lock);
	const bool ok = copy_latest(&bridge->h264, &bridge->h264_length, data, length);
	pthread_mutex_unlock(&bridge->lock);
	return ok;
}

void nanokvm_rdp_stop(NanokvmRdpBridge* bridge)
{
	if (!bridge)
		return;
	atomic_store(&bridge->running, false);
	if (bridge->thread_started)
	{
		pthread_join(bridge->thread, NULL);
		bridge->thread_started = false;
	}
	if (bridge->listener)
	{
		if (bridge->listener->Close)
			bridge->listener->Close(bridge->listener);
		freerdp_listener_free(bridge->listener);
		bridge->listener = NULL;
	}
	pthread_mutex_lock(&bridge->lock);
	free(bridge->bitmap);
	free(bridge->h264);
	pthread_mutex_unlock(&bridge->lock);
	pthread_mutex_destroy(&bridge->lock);
	free(bridge);
}
