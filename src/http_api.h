#ifndef NANOKVM_RDP_HTTP_API_H
#define NANOKVM_RDP_HTTP_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct AVFrame;

typedef struct HttpApi HttpApi;

typedef struct
{
	const char* bind_address;
	uint16_t port;
	uint16_t width;
	uint16_t height;
	int jpeg_quality;
	bool swap_alt_command;
	void* context;
	bool (*send_control)(void* context, uint8_t type, const void* payload, uint16_t length);
	bool (*set_stream)(void* context, bool requested);
	/* True while HTTP holds a pointer button or keyboard modifier. */
	bool input_busy;
} HttpApiConfig;

HttpApi* http_api_create(const HttpApiConfig* config);
void http_api_destroy(HttpApi* api);
bool http_api_start(HttpApi* api);
void http_api_stop(HttpApi* api);
bool http_api_input_idle(const HttpApi* api);
void http_api_set_desktop(HttpApi* api, uint16_t width, uint16_t height);

/* Encode and keep only the newest JPEG. Safe to call from the decoder thread. */
bool http_api_publish_frame(HttpApi* api, const struct AVFrame* frame);

#endif
