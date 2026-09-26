#ifndef NANOKVM_SESSION_API_H
#define NANOKVM_SESSION_API_H

#include "session_hooks.h"

#include <stdbool.h>
#include <stdint.h>

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

typedef struct NanokvmRdpSession NanokvmRdpSession;

NanokvmRdpSession* nanokvm_session_start(const NanokvmRdpConfig* config, const NanokvmSessionHooks* hooks);
bool nanokvm_session_pump(NanokvmRdpSession* session, uint32_t timeout_ms);
bool nanokvm_session_submit_bitmap(NanokvmRdpSession* session, const uint8_t* bgra, size_t length);
void nanokvm_session_stop(NanokvmRdpSession* session);

#endif
