#include "protocol.h"

#include <errno.h>
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
	uint8_t header[4] = { NANOKVM_PROTOCOL_VERSION, type, 0, 0 };
	if (length > NANOKVM_CONTROL_MAX_PAYLOAD)
		return false;
	protocol_write_u16(header + 2, length);
	return write_all(fd, header, sizeof(header)) &&
	       (length == 0 || write_all(fd, payload, length));
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
