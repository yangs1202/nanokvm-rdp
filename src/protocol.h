#ifndef NANOKVM_RDP_PROTOCOL_H
#define NANOKVM_RDP_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NANOKVM_CONTROL_MAX_PAYLOAD 1024U
#define NANOKVM_PROTOCOL_VERSION 1U
#define NANOKVM_STATS_PAYLOAD_SIZE 16U
#define NANOKVM_HELLO_BASE_PAYLOAD_SIZE 8U
#define NANOKVM_HELLO_CAPABILITIES_PAYLOAD_SIZE 9U
#define NANOKVM_KEY_PAYLOAD_SIZE 3U
#define NANOKVM_KEY_ACK_REQUEST_PAYLOAD_SIZE 7U
#define NANOKVM_KEY_ACK_PAYLOAD_SIZE 5U
#define NANOKVM_AGENT_CAPABILITY_KEY_ACK 0x01U

enum NanokvmControlType
{
	NANOKVM_CONTROL_HELLO = 1,
	NANOKVM_CONTROL_START_STREAM = 2,
	NANOKVM_CONTROL_STOP_STREAM = 3,
	NANOKVM_CONTROL_IDR_REQUEST = 4,
	NANOKVM_CONTROL_KEY = 5,
	NANOKVM_CONTROL_POINTER_ABS = 6,
	NANOKVM_CONTROL_POINTER_REL = 7,
	NANOKVM_CONTROL_WHEEL = 8,
	NANOKVM_CONTROL_RELEASE_ALL = 9,
	NANOKVM_CONTROL_PING = 10,
	NANOKVM_CONTROL_PONG = 11,
	NANOKVM_CONTROL_STATS = 12,
	NANOKVM_CONTROL_ERROR = 13,
	NANOKVM_CONTROL_TEXT_UTF8 = 14,
	NANOKVM_CONTROL_KEY_ACK = 15,
};

typedef struct
{
	uint8_t type;
	uint8_t payload[NANOKVM_CONTROL_MAX_PAYLOAD];
	uint16_t length;
} NanokvmControlMessage;

bool protocol_send(int fd, uint8_t type, const void* payload, uint16_t length);
bool protocol_receive(int fd, NanokvmControlMessage* message);
void protocol_write_u16(uint8_t* data, uint16_t value);
uint16_t protocol_read_u16(const uint8_t* data);
void protocol_write_u32(uint8_t* data, uint32_t value);
uint32_t protocol_read_u32(const uint8_t* data);

#endif
