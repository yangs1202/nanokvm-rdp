#include "rtp_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static bool append_access_unit(RtpClient* client, const uint8_t* data, size_t length)
{
	if (length > RTP_H264_MAX_NAL - client->access_unit_length)
		return false;
	const size_t needed = client->access_unit_length + length;
	if (needed > client->access_unit_capacity)
	{
		size_t capacity = client->access_unit_capacity ? client->access_unit_capacity : 4096U;
		while (capacity < needed)
			capacity *= 2U;
		uint8_t* resized = realloc(client->access_unit, capacity);
		if (!resized)
			return false;
		client->access_unit = resized;
		client->access_unit_capacity = capacity;
	}
	memcpy(client->access_unit + client->access_unit_length, data, length);
	client->access_unit_length += length;
	return true;
}

static bool save_nal(void* context, const uint8_t* nal, size_t length, uint32_t timestamp,
	                 bool marker)
{
	(void)marker;
	RtpClient* client = context;
	if (!client->have_access_unit || client->access_unit_timestamp != timestamp)
	{
		client->access_unit_length = 0;
		client->access_unit_timestamp = timestamp;
		client->have_access_unit = true;
	}
	const uint8_t start_code[] = { 0, 0, 0, 1 };
	return append_access_unit(client, start_code, sizeof(start_code)) &&
	       append_access_unit(client, nal, length);
}

static void count_loss(void* context)
{
	RtpClient* client = context;
	client->losses++;
	client->access_unit_length = 0;
	client->have_access_unit = false;
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
		if (!rtp_h264_reassembler_push(&client->reassembler, packet, (size_t)received, save_nal,
		                              client, count_loss, client))
			continue;
		if ((packet[1] & 0x80U) == 0 || client->access_unit_length == 0)
			continue;
		*data = client->access_unit;
		*length = client->access_unit_length;
		client->access_unit = NULL;
		client->access_unit_length = 0;
		client->access_unit_capacity = 0;
		client->have_access_unit = false;
		return true;
	}
}

void rtp_client_close(RtpClient* client)
{
	if (!client)
		return;
	if (client->fd >= 0)
		(void)close(client->fd);
	free(client->access_unit);
	rtp_h264_reassembler_free(&client->reassembler);
	*client = (RtpClient){ .fd = -1 };
}
