#include "rtp_client.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct
{
	int fd;
	struct sockaddr_in target;
} Sender;

static bool send_packet(void* context, const uint8_t* packet, size_t length)
{
	Sender* sender = context;
	return sendto(sender->fd, packet, length, 0, (const struct sockaddr*)&sender->target,
	              sizeof(sender->target)) == (ssize_t)length;
}

int main(void)
{
	RtpClient receiver = { .fd = socket(AF_INET, SOCK_DGRAM, 0) };
	Sender sender = { .fd = socket(AF_INET, SOCK_DGRAM, 0),
		.target = { .sin_family = AF_INET, .sin_port = 0,
		            .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) } } };
	assert(receiver.fd >= 0 && sender.fd >= 0);
	assert(bind(receiver.fd, (const struct sockaddr*)&sender.target, sizeof(sender.target)) == 0);
	socklen_t size = sizeof(sender.target);
	assert(getsockname(receiver.fd, (struct sockaddr*)&sender.target, &size) == 0);
	const struct timeval timeout = { .tv_sec = 1 };
	assert(setsockopt(receiver.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
	RtpH264Packetizer packetizer;
	rtp_h264_packetizer_init(&packetizer, RTP_H264_DEFAULT_MTU, 42);

	/* Queue an IDR and two dependent P frames before the reader gets CPU time.
	 * Include multiple NALs and fragmentation in the first access unit. */
	const uint8_t sps[] = { 0x67, 0x64, 0x00, 0x29 };
	uint8_t frames[3][1300];
	for (unsigned i = 0; i < 3; i++)
	{
		memset(frames[i], (int)i + 10, sizeof(frames[i]));
		frames[i][0] = i == 0 ? 0x65 : 0x41;
		if (i == 0)
			assert(rtp_h264_packetize_marker(&packetizer, sps, sizeof(sps), i * 3000U,
			                                false, send_packet, &sender));
		assert(rtp_h264_packetize(&packetizer, frames[i], sizeof(frames[i]), i * 3000U,
		                         send_packet, &sender));
	}
	for (unsigned i = 0; i < 3; i++)
	{
		uint8_t* data = NULL;
		size_t length = 0;
		assert(rtp_client_read_h264(&receiver, &data, &length));
		const size_t prefix = i == 0 ? sizeof(sps) + 4U : 0;
		assert(length == prefix + 4U + sizeof(frames[i]));
		const uint8_t start_code[] = { 0, 0, 0, 1 };
		if (i == 0)
		{
			assert(memcmp(data, start_code, 4) == 0);
			assert(memcmp(data + 4, sps, sizeof(sps)) == 0);
		}
		assert(memcmp(data + prefix, start_code, 4) == 0);
		assert(memcmp(data + prefix + 4, frames[i], sizeof(frames[i])) == 0);
		free(data);
	}
	uint8_t* data = NULL;
	size_t length = 0;
	assert(!rtp_client_read_h264(&receiver, &data, &length));
	assert(errno == EAGAIN || errno == EWOULDBLOCK);
	assert(data == NULL && length == 0 && receiver.losses == 0);
	rtp_client_close(&receiver);
	assert(close(sender.fd) == 0);
	puts("RTP client: queued IDR/P frames, multi-NAL and fragmented access units preserved");
	return 0;
}
