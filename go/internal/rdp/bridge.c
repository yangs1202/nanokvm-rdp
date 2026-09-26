#include "bridge.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

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
};

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
	return bridge;
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
	pthread_mutex_lock(&bridge->lock);
	free(bridge->bitmap);
	free(bridge->h264);
	pthread_mutex_unlock(&bridge->lock);
	pthread_mutex_destroy(&bridge->lock);
	free(bridge);
}
