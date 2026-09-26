#ifndef NANOKVM_RDP_SHIM_H
#define NANOKVM_RDP_SHIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NanokvmRdpShim NanokvmRdpShim;

enum NanokvmRdpShimEvent
{
	NANOKVM_RDP_EVENT_SESSION_STARTED = 1,
	NANOKVM_RDP_EVENT_SESSION_STOPPED = 2,
	NANOKVM_RDP_EVENT_VIDEO_AVC420 = 3,
	NANOKVM_RDP_EVENT_VIDEO_BITMAP = 4,
	NANOKVM_RDP_EVENT_FRAME_ACK = 5,
	NANOKVM_RDP_EVENT_RDP_ERROR = 6,
	NANOKVM_RDP_EVENT_H264_QUEUE_OVERFLOW = 7,
};

/* The owner is an integer handle, never a Go pointer. The shim may retain it
 * until nanokvm_rdp_shim_free returns. */
typedef struct
{
	uintptr_t owner;
	void (*event)(uintptr_t owner, uint32_t event, uint32_t value, uint32_t value2);
	uint8_t (*input)(uintptr_t owner, uint8_t type, uint8_t* payload, uint16_t length);
} NanokvmRdpCallbacks;

NanokvmRdpShim* nanokvm_rdp_shim_new(const char* bind_address, uint16_t port,
	                                 const char* certificate, const char* private_key,
	                                 uint16_t width, uint16_t height, bool direct_gfx,
	                                 const NanokvmRdpCallbacks* callbacks);
int nanokvm_rdp_shim_run(NanokvmRdpShim* shim);
void nanokvm_rdp_shim_stop(NanokvmRdpShim* shim);
void nanokvm_rdp_shim_disconnect_active(NanokvmRdpShim* shim);
void nanokvm_rdp_shim_free(NanokvmRdpShim* shim);

/* BGRA is latest-only. H.264 access units are ordered and bounded because
 * P-frames depend on earlier pictures. Overflow drops the queue, emits an
 * H264_QUEUE_OVERFLOW event, and ignores P-frames until a keyframe arrives. */
bool nanokvm_rdp_shim_send_h264(NanokvmRdpShim* shim, const uint8_t* data, size_t length,
	                            bool keyframe);
bool nanokvm_rdp_shim_send_bgra(NanokvmRdpShim* shim, const uint8_t* data, size_t length);

#ifdef __cplusplus
}
#endif

#endif
