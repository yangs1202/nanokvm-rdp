#ifndef NANOKVM_RDP_RTP_CLIENT_H
#define NANOKVM_RDP_RTP_CLIENT_H

#include "rtp_h264.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
	int fd;
	RtpH264Reassembler reassembler;
	uint8_t* nal;
	size_t nal_length;
	uint32_t losses;
} RtpClient;

bool rtp_client_open(RtpClient* client, uint16_t port);
bool rtp_client_read_h264(RtpClient* client, uint8_t** data, size_t* length);
void rtp_client_close(RtpClient* client);

#endif
