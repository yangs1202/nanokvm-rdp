#ifndef NANOKVM_GATEWAY_CONFIG_H
#define NANOKVM_GATEWAY_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct
{
	char bind_address[64];
	uint16_t port;
	const char* certificate;
	const char* private_key;
	uint16_t width;
	uint16_t height;
	uint16_t bitrate;
	uint16_t control_port;
	uint16_t video_port;
	uint16_t http_port;
	int jpeg_quality;
	bool direct_gfx;
	bool swap_alt_command;
} GatewayConfig;

void gateway_config_defaults(GatewayConfig* config);
bool gateway_config_parse(GatewayConfig* config, int argc, char* const argv[], char* error,
			  size_t error_size);
void gateway_config_print_usage(FILE* stream, const char* executable);

#endif
