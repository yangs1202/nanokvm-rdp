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

typedef struct
{
	Sender* sender;
	unsigned packets;
} FirstPacketSender;

static bool send_packet(void* context, const uint8_t* packet, size_t length)
{
	Sender* sender = context;
	return sendto(sender->fd, packet, length, 0, (const struct sockaddr*)&sender->target,
	              sizeof(sender->target)) == (ssize_t)length;
}

static bool send_first_packet(void* context, const uint8_t* packet, size_t length)
{
	FirstPacketSender* first = context;
	if (first->packets++ == 0)
		assert(send_packet(first->sender, packet, length));
	return false;
}

static uint16_t available_port(void)
{
	const int fd = socket(AF_INET, SOCK_DGRAM, 0);
	assert(fd >= 0);
	struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_port = 0,
		.sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) },
	};
	assert(bind(fd, (const struct sockaddr*)&address, sizeof(address)) == 0);
	socklen_t size = sizeof(address);
	assert(getsockname(fd, (struct sockaddr*)&address, &size) == 0);
	assert(close(fd) == 0);
	return ntohs(address.sin_port);
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
		assert(receiver.access_unit_ssrc == 42);
		assert(receiver.access_unit_timestamp == i * 3000U);
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

	/* Release/reacquire closes and reopens RtpClient. Prove that doing so
	 * discards both an in-progress FU-A and a complete queued datagram, then
	 * accepts a reset sequence from a different SSRC without reporting loss. */
	const uint16_t reset_port = available_port();
	RtpClient reset_receiver = { .fd = -1 };
	assert(rtp_client_open(&reset_receiver, reset_port));
	sender.target.sin_port = htons(reset_port);
	RtpH264Packetizer old_stream;
	rtp_h264_packetizer_init(&old_stream, RTP_H264_DEFAULT_MTU, 42);
	uint8_t partial_idr[1300];
	memset(partial_idr, 0xaa, sizeof(partial_idr));
	partial_idr[0] = 0x65;
	FirstPacketSender first = { .sender = &sender };
	assert(!rtp_h264_packetize(&old_stream, partial_idr, sizeof(partial_idr), 6000,
	                           send_first_packet, &first));
	assert(first.packets == 1);
	data = NULL;
	length = 0;
	assert(!rtp_client_read_h264(&reset_receiver, &data, &length));
	assert(reset_receiver.reassembler.assembling && data == NULL && length == 0);
	const uint8_t stale_p[] = { 0x41, 0xde, 0xad };
	assert(rtp_h264_packetize(&old_stream, stale_p, sizeof(stale_p), 7000,
	                         send_packet, &sender));
	rtp_client_close(&reset_receiver);

	assert(rtp_client_open(&reset_receiver, reset_port));
	RtpH264Packetizer fresh_stream;
	rtp_h264_packetizer_init(&fresh_stream, RTP_H264_DEFAULT_MTU, 84);
	const uint8_t fresh_sync[] = {
		0, 0, 0, 1, 0x67, 0x64,
		0, 0, 0, 1, 0x68, 0xee,
		0, 0, 0, 1, 0x65, 0xbe, 0xef,
	};
	assert(rtp_h264_packetize_access_unit(&fresh_stream, fresh_sync, sizeof(fresh_sync), 9000,
	                                      send_packet, &sender));
	assert(rtp_client_read_h264(&reset_receiver, &data, &length));
	assert(reset_receiver.access_unit_ssrc == 84);
	assert(reset_receiver.access_unit_timestamp == 9000 && reset_receiver.losses == 0);
	assert(length == sizeof(fresh_sync) && memcmp(data, fresh_sync, sizeof(fresh_sync)) == 0);
	free(data);
	rtp_client_close(&reset_receiver);
	assert(close(sender.fd) == 0);
	puts("RTP client: reconnect discards stale UDP/partial AU and accepts reset SSRC/sequence");
	return 0;
}
