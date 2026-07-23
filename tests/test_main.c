#include "h264.h"
#include "hid.h"
#include "protocol.h"
#include "rtp_h264.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct
{
	uint8_t packets[8][RTP_H264_DEFAULT_MTU];
	size_t lengths[8];
	size_t count;
	uint8_t output[RTP_H264_MAX_NAL];
	size_t output_length;
	unsigned losses;
} RtpTestState;

static bool collect_packet(void* context, const uint8_t* packet, size_t length)
{
	RtpTestState* state = context;
	assert(state->count < 8);
	memcpy(state->packets[state->count], packet, length);
	state->lengths[state->count++] = length;
	return true;
}

static bool collect_nal(void* context, const uint8_t* nal, size_t length, uint32_t timestamp,
	                    bool marker)
{
	RtpTestState* state = context;
	assert(timestamp == 90000U);
	memcpy(state->output, nal, length);
	state->output_length = length;
	assert(marker);
	return true;
}

static void count_loss(void* context)
{
	((RtpTestState*)context)->losses++;
}

static void test_h264_annexb(void)
{
	const uint8_t idr[] = { 0, 0, 0, 1, 0x65, 0x88 };
	const uint8_t sps[] = { 0x67, 0x42, 0x00 };
	uint8_t copied[8] = { 0 };
	assert(h264_has_annexb_start_code(idr, sizeof(idr)));
	assert(h264_contains_nal_type(idr, sizeof(idr), 5));
	assert(!h264_contains_nal_type(idr, sizeof(idr), 7));
	assert(h264_annexb_size(sps, sizeof(sps)) == sizeof(sps) + 4);
	assert(h264_copy_annexb(copied, sps, sizeof(sps)) == sizeof(sps) + 4);
	assert(copied[0] == 0 && copied[1] == 0 && copied[2] == 0 && copied[3] == 1);
	assert(copied[4] == 0x67);
}

static void test_hid_mapping(void)
{
	uint8_t usage = 0;
	uint8_t modifier = 0;
	assert(hid_translate_scancode(0x1e, false, &usage, &modifier));
	assert(usage == 0x04 && modifier == 0);
	assert(hid_translate_scancode(0x1d, true, &usage, &modifier));
	assert(usage == 0 && modifier == 0x10);
	assert(hid_translate_scancode(0x4b, true, &usage, &modifier));
	assert(usage == 0x50 && modifier == 0);
	assert(hid_scale_absolute(0, 1920) == 1);
	assert(hid_scale_absolute(1919, 1920) == 0x7fff);
	assert(hid_clamp_absolute(1200, 1920) == 1200);
	assert(hid_clamp_absolute(3000, 1920) == 1919);
}

static void test_protocol_primitives(void)
{
	uint8_t bytes[4] = { 0 };
	protocol_write_u16(bytes, 0xabcdU);
	assert(protocol_read_u16(bytes) == 0xabcdU);
	protocol_write_u32(bytes, 0x89abcdefU);
	assert(protocol_read_u32(bytes) == 0x89abcdefU);
}

static void test_control_wire_message(void)
{
	int sockets[2] = { -1, -1 };
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	const uint8_t payload[] = { 0x1e, 1, 0 };
	NanokvmControlMessage received = { 0 };
	assert(protocol_send(sockets[0], NANOKVM_CONTROL_KEY, payload, sizeof(payload)));
	assert(protocol_receive(sockets[1], &received));
	assert(received.type == NANOKVM_CONTROL_KEY);
	assert(received.length == sizeof(payload));
	assert(memcmp(received.payload, payload, sizeof(payload)) == 0);
	assert(close(sockets[0]) == 0);
	assert(close(sockets[1]) == 0);
}

static void test_rtp_h264_fragmentation_and_loss(void)
{
	uint8_t nal[3000] = { 0x65 };
	for (size_t index = 1; index < sizeof(nal); index++)
		nal[index] = (uint8_t)index;
	RtpH264Packetizer packetizer = { 0 };
	RtpH264Reassembler reassembler = { 0 };
	RtpTestState state = { 0 };
	rtp_h264_packetizer_init(&packetizer, 1200, 7);
	assert(rtp_h264_packetize(&packetizer, nal, sizeof(nal), 90000U, collect_packet, &state));
	assert(state.count == 3);
	rtp_h264_reassembler_init(&reassembler);
	for (size_t index = 0; index < state.count; index++)
		assert(rtp_h264_reassembler_push(&reassembler, state.packets[index], state.lengths[index],
		                                 collect_nal, &state, count_loss, &state));
	assert(state.output_length == sizeof(nal));
	assert(memcmp(state.output, nal, sizeof(nal)) == 0);
	assert(state.losses == 0);

	rtp_h264_reassembler_free(&reassembler);
	rtp_h264_reassembler_init(&reassembler);
	state.output_length = 0;
	assert(rtp_h264_reassembler_push(&reassembler, state.packets[0], state.lengths[0], collect_nal,
	                                 &state, count_loss, &state));
	assert(!rtp_h264_reassembler_push(&reassembler, state.packets[2], state.lengths[2], collect_nal,
	                                  &state, count_loss, &state));
	assert(state.losses == 1);
	assert(state.output_length == 0);
	rtp_h264_reassembler_free(&reassembler);
}

int main(void)
{
	test_h264_annexb();
	test_hid_mapping();
	test_protocol_primitives();
	test_control_wire_message();
	test_rtp_h264_fragmentation_and_loss();
	return 0;
}
