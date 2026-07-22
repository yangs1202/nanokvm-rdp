#include "foldvnc_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define FOLDVNC_MAGIC 0x464eU
#define FOLDVNC_MAX_PAYLOAD (8U * 1024U * 1024U)
#define FOLDVNC_TYPE_HELLO 0x01U
#define FOLDVNC_TYPE_HELLO_ACK 0x02U
#define FOLDVNC_TYPE_CONFIG 0x03U
#define FOLDVNC_TYPE_CONFIG_ACK 0x04U
#define FOLDVNC_TYPE_VIDEO_FRAME 0x10U
#define FOLDVNC_CODEC_H264 0x01U

typedef struct
{
	uint8_t type;
	uint8_t flags;
	uint8_t* payload;
	uint32_t length;
} FoldVncMessage;

static uint16_t read_u16(const uint8_t* data)
{
	return ((uint16_t)data[0] << 8U) | data[1];
}

static uint32_t read_u32(const uint8_t* data)
{
	return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) |
	       ((uint32_t)data[2] << 8U) | data[3];
}

static void write_u16(uint8_t* data, uint16_t value)
{
	data[0] = (uint8_t)(value >> 8U);
	data[1] = (uint8_t)value;
}

static void write_u32(uint8_t* data, uint32_t value)
{
	data[0] = (uint8_t)(value >> 24U);
	data[1] = (uint8_t)(value >> 16U);
	data[2] = (uint8_t)(value >> 8U);
	data[3] = (uint8_t)value;
}

static bool write_all(int fd, const uint8_t* data, size_t length)
{
	while (length > 0)
	{
		const ssize_t written = write(fd, data, length);
		if (written > 0)
		{
			data += written;
			length -= (size_t)written;
			continue;
		}
		if (written < 0 && errno == EINTR)
			continue;
		return false;
	}
	return true;
}

static bool read_all(int fd, uint8_t* data, size_t length)
{
	while (length > 0)
	{
		const ssize_t received = read(fd, data, length);
		if (received > 0)
		{
			data += received;
			length -= (size_t)received;
			continue;
		}
		if (received < 0 && errno == EINTR)
			continue;
		return false;
	}
	return true;
}

static bool send_message(int fd, uint8_t type, uint8_t flags, const uint8_t* payload,
	                     uint32_t length)
{
	uint8_t header[8] = { 0 };
	write_u16(header, FOLDVNC_MAGIC);
	header[2] = type;
	header[3] = flags;
	write_u32(header + 4, length);
	return write_all(fd, header, sizeof(header)) && (length == 0 || write_all(fd, payload, length));
}

static bool read_message(int fd, FoldVncMessage* message)
{
	uint8_t header[8] = { 0 };
	if (!read_all(fd, header, sizeof(header)) || read_u16(header) != FOLDVNC_MAGIC)
		return false;
	message->type = header[2];
	message->flags = header[3];
	message->length = read_u32(header + 4);
	if (message->length > FOLDVNC_MAX_PAYLOAD)
		return false;
	if (message->length == 0)
		return true;
	message->payload = malloc(message->length);
	return message->payload && read_all(fd, message->payload, message->length);
}

static void free_message(FoldVncMessage* message)
{
	free(message->payload);
	*message = (FoldVncMessage){ 0 };
}

static bool expect_message(int fd, uint8_t expected_type, FoldVncMessage* result)
{
	for (;;)
	{
		FoldVncMessage message = { 0 };
		if (!read_message(fd, &message))
			return false;
		if (message.type == expected_type)
		{
			*result = message;
			return true;
		}
		free_message(&message);
	}
}

bool foldvnc_client_connect(FoldVncClient* client, const char* host, uint16_t port, uint16_t width,
	                        uint16_t height, uint8_t fps)
{
	if (!client || !host || width == 0 || height == 0 || fps == 0)
		return false;
	*client = (FoldVncClient){ .fd = -1 };
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return false;
	struct sockaddr_in address = { 0 };
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	if (inet_pton(AF_INET, host, &address.sin_addr) != 1 ||
	    connect(fd, (const struct sockaddr*)&address, sizeof(address)) != 0)
		goto fail;

	static const char client_name[] = "NanoKVM-RDP/1.0";
	uint8_t hello[3 + sizeof(client_name) - 1] = { 1, 0, sizeof(client_name) - 1 };
	memcpy(hello + 3, client_name, sizeof(client_name) - 1);
	if (!send_message(fd, FOLDVNC_TYPE_HELLO, 0, hello, sizeof(hello)))
		goto fail;
	FoldVncMessage message = { 0 };
	if (!expect_message(fd, FOLDVNC_TYPE_HELLO_ACK, &message))
		goto fail;
	free_message(&message);

	uint8_t config[6] = { 0 };
	write_u16(config, width);
	write_u16(config + 2, height);
	config[4] = fps;
	config[5] = FOLDVNC_CODEC_H264;
	if (!send_message(fd, FOLDVNC_TYPE_CONFIG, 0, config, sizeof(config)) ||
	    !expect_message(fd, FOLDVNC_TYPE_CONFIG_ACK, &message))
		goto fail;
	if (message.length < 8 || read_u16(message.payload) != width ||
	    read_u16(message.payload + 2) != height || message.payload[5] != FOLDVNC_CODEC_H264)
	{
		free_message(&message);
		goto fail;
	}
	free_message(&message);
	client->fd = fd;
	client->width = width;
	client->height = height;
	return true;

fail:
	(void)close(fd);
	return false;
}

bool foldvnc_client_read_video(FoldVncClient* client, uint8_t** data, size_t* length,
	                           bool* keyframe)
{
	if (!client || client->fd < 0 || !data || !length || !keyframe)
		return false;
	*data = NULL;
	*length = 0;
	*keyframe = false;
	for (;;)
	{
		FoldVncMessage message = { 0 };
		if (!read_message(client->fd, &message))
			return false;
		if (message.type != FOLDVNC_TYPE_VIDEO_FRAME)
		{
			free_message(&message);
			continue;
		}
		if (message.length <= 12)
		{
			free_message(&message);
			return false;
		}
		const size_t video_length = message.length - 12U;
		uint8_t* video = malloc(video_length);
		if (!video)
		{
			free_message(&message);
			return false;
		}
		memcpy(video, message.payload + 12, video_length);
		*keyframe = (message.flags & 1U) != 0;
		*data = video;
		*length = video_length;
		free_message(&message);
		return true;
	}
}

void foldvnc_client_close(FoldVncClient* client)
{
	if (!client)
		return;
	if (client->fd >= 0)
	{
		(void)shutdown(client->fd, SHUT_RDWR);
		(void)close(client->fd);
	}
	*client = (FoldVncClient){ .fd = -1 };
}
