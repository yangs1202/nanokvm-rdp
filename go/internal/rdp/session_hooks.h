#ifndef NANOKVM_SESSION_HOOKS_H
#define NANOKVM_SESSION_HOOKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NANOKVM_CONTROL_START_STREAM 2U
#define NANOKVM_CONTROL_STOP_STREAM 3U
#define NANOKVM_CONTROL_IDR_REQUEST 4U
#define NANOKVM_CONTROL_KEY 5U
#define NANOKVM_CONTROL_POINTER_ABS 6U
#define NANOKVM_CONTROL_POINTER_REL 7U
#define NANOKVM_CONTROL_WHEEL 8U
#define NANOKVM_CONTROL_RELEASE_ALL 9U
#define NANOKVM_CONTROL_TEXT_UTF8 14U
#define NANOKVM_CONTROL_SYNCHRONIZE 16U

typedef struct
{
	void* context;
	bool (*send_control)(void* context, uint8_t type, const void* payload, uint16_t length);
	bool (*read_h264)(void* context, uint8_t** data, size_t* length, uint32_t* losses);
	bool (*decode_start)(void* context, uint16_t width, uint16_t height);
	bool (*decode_push)(void* context, const uint8_t* data, size_t length);
	void (*decode_stop)(void* context);
} NanokvmSessionHooks;

#endif
