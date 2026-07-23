#include "rtp_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static bool save_nal(void* context, const uint8_t* nal, size_t length, uint32_t timestamp)
{
	(void)timestamp;
	RtpClient* client = context;
	uint8_t* copied = malloc(length);
	if (!copied)
		return false;
	memcpy(copied, nal, length);
	free(client->nal);
	client->nal = copied;
	client->nal_length = length;
	return true;
}

static void count_loss(void* context)
{
	((RtpClient*)context)->losses++;
}

bool rtp_client_open(RtpClient* client, uint16_t port)
{
	if (!client || port == 0)
		return false;
	*client = (RtpClient){ .fd = -1 };
	client->fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (client->fd < 0)
		return false;
	int enabled = 1;
	(void)setsockopt(client->fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
	struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(port),
		.sin_addr = { .s_addr = htonl(INADDR_ANY) } };
	if (bind(client->fd, (const struct sockaddr*)&address, sizeof(address)) != 0)
	{
		(void)close(client->fd);
		client->fd = -1;
		return false;
	}
	rtp_h264_reassembler_init(&client->reassembler);
	return true;
}

bool rtp_client_read_h264(RtpClient* client, uint8_t** data, size_t* length)
{
	if (!client || client->fd < 0 || !data || !length)
		return false;
	*data = NULL;
	*length = 0;
	for (;;)
	{
		uint8_t packet[1500] = { 0 };
		const ssize_t received = recv(client->fd, packet, sizeof(packet), 0);
		if (received < 0 && errno == EINTR)
			continue;
		if (received <= 0)
			return false;
		free(client->nal);
		client->nal = NULL;
		client->nal_length = 0;
		if (!rtp_h264_reassembler_push(&client->reassembler, packet, (size_t)received, save_nal,
		                              client, count_loss, client))
			continue;
		if (!client->nal)
			continue;
		*data = client->nal;
		*length = client->nal_length;
		client->nal = NULL;
		client->nal_length = 0;
		return true;
	}
}

void rtp_client_close(RtpClient* client)
{
	if (!client)
		return;
	if (client->fd >= 0)
		(void)close(client->fd);
	free(client->nal);
	rtp_h264_reassembler_free(&client->reassembler);
	*client = (RtpClient){ .fd = -1 };
}
