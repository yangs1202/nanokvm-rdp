#include "protocol.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static bool write_all(int fd, const uint8_t* data, size_t length)
{
	while (length > 0)
	{
		const ssize_t result = write(fd, data, length);
		if (result > 0)
		{
			data += result;
			length -= (size_t)result;
			continue;
		}
		if (result < 0 && errno == EINTR)
			continue;
		return false;
	}
	return true;
}

static bool read_all(int fd, uint8_t* data, size_t length)
{
	while (length > 0)
	{
		const ssize_t result = read(fd, data, length);
		if (result > 0)
		{
			data += result;
			length -= (size_t)result;
			continue;
		}
		if (result < 0 && errno == EINTR)
			continue;
		return false;
	}
	return true;
}

void protocol_write_u16(uint8_t* data, uint16_t value)
{
	data[0] = (uint8_t)(value >> 8U);
	data[1] = (uint8_t)value;
}

uint16_t protocol_read_u16(const uint8_t* data)
{
	return ((uint16_t)data[0] << 8U) | data[1];
}

void protocol_write_u32(uint8_t* data, uint32_t value)
{
	data[0] = (uint8_t)(value >> 24U);
	data[1] = (uint8_t)(value >> 16U);
	data[2] = (uint8_t)(value >> 8U);
	data[3] = (uint8_t)value;
}

uint32_t protocol_read_u32(const uint8_t* data)
{
	return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) |
	       ((uint32_t)data[2] << 8U) | data[3];
}

bool protocol_send(int fd, uint8_t type, const void* payload, uint16_t length)
{
	uint8_t frame[4 + NANOKVM_CONTROL_MAX_PAYLOAD] = { NANOKVM_PROTOCOL_VERSION, type, 0, 0 };
	if (length > NANOKVM_CONTROL_MAX_PAYLOAD || (length && !payload))
		return false;
	protocol_write_u16(frame + 2, length);
	if (length)
		memcpy(frame + 4, payload, length);
	if (write_all(fd, frame, 4U + length))
		return true;
	/* A partially written frame cannot be followed by another frame safely. */
	(void)shutdown(fd, SHUT_RDWR);
	return false;
}

int protocol_receive_available(int fd, NanokvmControlReader* reader, NanokvmControlMessage* message)
{
	for (;;)
	{
		size_t needed = 4;
		if (reader->used >= 4)
		{
			const uint16_t length = protocol_read_u16(reader->data + 2);
			if (reader->data[0] != NANOKVM_PROTOCOL_VERSION || length > NANOKVM_CONTROL_MAX_PAYLOAD)
				return -1;
			needed += length;
			if (reader->used == needed)
			{
				message->type = reader->data[1];
				message->length = length;
				memcpy(message->payload, reader->data + 4, length);
				reader->used = 0;
				return 1;
			}
		}
		const ssize_t got = recv(fd, reader->data + reader->used, needed - reader->used, MSG_DONTWAIT);
		if (got > 0)
			reader->used += (size_t)got;
		else if (got < 0 && errno == EINTR)
			continue;
		else
			return got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
	}
}

bool protocol_receive(int fd, NanokvmControlMessage* message)
{
	uint8_t header[4] = { 0 };
	if (!message || !read_all(fd, header, sizeof(header)) || header[0] != NANOKVM_PROTOCOL_VERSION)
		return false;
	message->type = header[1];
	message->length = protocol_read_u16(header + 2);
	if (message->length > NANOKVM_CONTROL_MAX_PAYLOAD)
		return false;
	return message->length == 0 || read_all(fd, message->payload, message->length);
}
