#ifndef NANOKVM_RDP_BRIDGE_H
#define NANOKVM_RDP_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct NanokvmRdpBridge NanokvmRdpBridge;

typedef struct
{
	const char* bind_address;
	uint16_t port;
	const char* certificate;
	const char* private_key;
	uint16_t width;
	uint16_t height;
	bool direct_gfx;
	bool swap_alt_command;
} NanokvmRdpConfig;

typedef struct
{
	uint8_t kind;
	uint16_t flags;
	uint16_t code;
	int16_t x;
	int16_t y;
} NanokvmRdpInput;

typedef void (*NanokvmRdpInputCallback)(void* context, const NanokvmRdpInput* input);

NanokvmRdpBridge* nanokvm_rdp_start(const NanokvmRdpConfig* config, NanokvmRdpInputCallback callback,
                                    void* context);
bool nanokvm_rdp_submit_bgra(NanokvmRdpBridge* bridge, const uint8_t* bgra, size_t length,
                             uint16_t width, uint16_t height);
bool nanokvm_rdp_submit_h264(NanokvmRdpBridge* bridge, const uint8_t* data, size_t length);
void nanokvm_rdp_stop(NanokvmRdpBridge* bridge);

#endif
