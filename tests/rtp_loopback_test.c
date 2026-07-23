#include "rtp_h264.h"

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct
{
	int fd;
	struct sockaddr_in target;
} Sender;

typedef struct
{
	uint8_t data[2048];
	size_t length;
} Receiver;

static bool send_packet(void* context, const uint8_t* packet, size_t length)
{
	Sender* sender = context;
	return sendto(sender->fd, packet, length, 0, (const struct sockaddr*)&sender->target,
	              sizeof(sender->target)) == (ssize_t)length;
}

static bool receive_nal(void* context, const uint8_t* nal, size_t length, uint32_t timestamp)
{
	Receiver* receiver = context;
	assert(timestamp == 1234U);
	memcpy(receiver->data, nal, length);
	receiver->length = length;
	return true;
}

int main(void)
{
	const int receiver_fd = socket(AF_INET, SOCK_DGRAM, 0);
	const int sender_fd = socket(AF_INET, SOCK_DGRAM, 0);
	assert(receiver_fd >= 0 && sender_fd >= 0);
	struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = 0,
		.sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) } };
	assert(bind(receiver_fd, (const struct sockaddr*)&local, sizeof(local)) == 0);
	socklen_t local_length = sizeof(local);
	assert(getsockname(receiver_fd, (struct sockaddr*)&local, &local_length) == 0);
	Sender sender = { .fd = sender_fd, .target = local };
	uint8_t nal[1300] = { 0x65 };
	for (size_t index = 1; index < sizeof(nal); index++)
		nal[index] = (uint8_t)index;
	RtpH264Packetizer packetizer = { 0 };
	rtp_h264_packetizer_init(&packetizer, RTP_H264_DEFAULT_MTU, 42);
	assert(rtp_h264_packetize(&packetizer, nal, sizeof(nal), 1234U, send_packet, &sender));
	RtpH264Reassembler reassembler = { 0 };
	Receiver receiver = { 0 };
	rtp_h264_reassembler_init(&reassembler);
	while (receiver.length == 0)
	{
		uint8_t packet[1500] = { 0 };
		const ssize_t received = recv(receiver_fd, packet, sizeof(packet), 0);
		assert(received > 0);
		assert(rtp_h264_reassembler_push(&reassembler, packet, (size_t)received, receive_nal,
		                                 &receiver, NULL, NULL));
	}
	assert(receiver.length == sizeof(nal));
	assert(memcmp(receiver.data, nal, sizeof(nal)) == 0);
	rtp_h264_reassembler_free(&reassembler);
	assert(close(sender_fd) == 0);
	assert(close(receiver_fd) == 0);
	return 0;
}
