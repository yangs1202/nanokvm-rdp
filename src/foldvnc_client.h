#ifndef NANOKVM_RDP_FOLDVNC_CLIENT_H
#define NANOKVM_RDP_FOLDVNC_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
	int fd;
	uint16_t width;
	uint16_t height;
} FoldVncClient;

bool foldvnc_client_connect(FoldVncClient* client, const char* host, uint16_t port, uint16_t width,
	                        uint16_t height, uint8_t fps);
bool foldvnc_client_read_video(FoldVncClient* client, uint8_t** data, size_t* length,
	                           bool* keyframe);
void foldvnc_client_close(FoldVncClient* client);

#endif
