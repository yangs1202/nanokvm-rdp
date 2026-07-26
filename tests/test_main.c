#include "h264.h"
#include "hid.h"
#include "protocol.h"
#include "rtp_h264.h"

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

typedef struct
{
	uint8_t output[2][16];
	size_t lengths[2];
	bool markers[2];
	size_t count;
} RtpStapTestState;

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

static bool collect_stap_nal(void* context, const uint8_t* nal, size_t length,
	                         uint32_t timestamp, bool marker)
{
	RtpStapTestState* state = context;
	assert(timestamp == 90000U);
	assert(state->count < 2);
	assert(length <= sizeof(state->output[state->count]));
	memcpy(state->output[state->count], nal, length);
	state->lengths[state->count] = length;
	state->markers[state->count] = marker;
	state->count++;
	return true;
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

static void test_hid_middle_button(void)
{
	char touch_path[] = "/tmp/nanokvm-rdp-touch-XXXXXX";
	const int temp_fd = mkstemp(touch_path);
	assert(temp_fd >= 0);
	assert(close(temp_fd) == 0);

	HidState hid;
	hid_init(&hid, "/dev/null", "/dev/null", touch_path);
	assert(hid_absolute(&hid, 100, 200, 1920, 1080, 0xa000U));

	int read_fd = open(touch_path, O_RDONLY);
	assert(read_fd >= 0);
	uint8_t report[6] = { 0 };
	assert(read(read_fd, report, sizeof(report)) == (ssize_t)sizeof(report));
	assert(report[0] == 0x02);
	assert(close(read_fd) == 0);

	assert(hid_absolute(&hid, 100, 200, 1920, 1080, 0x2000U));
	assert(hid_absolute(&hid, 100, 200, 1920, 1080, 0xc000U));

	read_fd = open(touch_path, O_RDONLY);
	assert(read_fd >= 0);
	memset(report, 0, sizeof(report));
	assert(read(read_fd, report, sizeof(report)) == (ssize_t)sizeof(report));
	assert(report[0] == 0x04);
	assert(close(read_fd) == 0);
	assert(unlink(touch_path) == 0);
}

static void test_hid_extended_buttons(void)
{
	char mouse_path[] = "/tmp/nanokvm-rdp-mouse-XXXXXX";
	char touch_path[] = "/tmp/nanokvm-rdp-touch-XXXXXX";
	const int mouse_fd = mkstemp(mouse_path);
	const int touch_fd = mkstemp(touch_path);
	assert(mouse_fd >= 0 && touch_fd >= 0);
	assert(close(mouse_fd) == 0);
	assert(close(touch_fd) == 0);

	HidState hid;
	hid_init(&hid, "/dev/null", mouse_path, touch_path);
	assert(hid_absolute(&hid, 100, 200, 1920, 1080, 0x8001U));
	int read_fd = open(touch_path, O_RDONLY);
	assert(read_fd >= 0);
	uint8_t report[6] = { 0 };
	assert(read(read_fd, report, sizeof(report)) == (ssize_t)sizeof(report));
	assert(report[0] == 0x08 && report[5] == 0);
	assert(close(read_fd) == 0);

	assert(hid_wheel(&hid, 0x0278U));
	read_fd = open(touch_path, O_RDONLY);
	assert(read_fd >= 0);
	uint8_t wheel_report[6] = { 0 };
	assert(read(read_fd, wheel_report, sizeof(wheel_report)) == (ssize_t)sizeof(wheel_report));
	assert(wheel_report[0] == 0x08 && wheel_report[5] == 1);
	assert(close(read_fd) == 0);

	assert(hid_absolute(&hid, 100, 200, 1920, 1080, 0));
	read_fd = open(touch_path, O_RDONLY);
	assert(read_fd >= 0);
	memset(report, 0, sizeof(report));
	assert(read(read_fd, report, sizeof(report)) == (ssize_t)sizeof(report));
	assert(report[0] == 0x08 && report[5] == 0);
	assert(close(read_fd) == 0);

	assert(hid_absolute(&hid, 100, 200, 1920, 1080, 0x0001U));
	read_fd = open(touch_path, O_RDONLY);
	assert(read_fd >= 0);
	memset(report, 0, sizeof(report));
	assert(read(read_fd, report, sizeof(report)) == (ssize_t)sizeof(report));
	assert(report[0] == 0x00 && report[5] == 0);
	assert(close(read_fd) == 0);

	assert(hid_absolute(&hid, 100, 200, 1920, 1080, 0x8002U));
	read_fd = open(touch_path, O_RDONLY);
	assert(read_fd >= 0);
	memset(report, 0, sizeof(report));
	assert(read(read_fd, report, sizeof(report)) == (ssize_t)sizeof(report));
	assert(report[0] == 0x10 && report[5] == 0);
	assert(close(read_fd) == 0);
	assert(unlink(mouse_path) == 0);
	assert(unlink(touch_path) == 0);
}

static void test_hid_text_utf8(void)
{
	char keyboard_path[] = "/tmp/nanokvm-rdp-keyboard-XXXXXX";
	const int temp_fd = mkstemp(keyboard_path);
	assert(temp_fd >= 0);
	assert(close(temp_fd) == 0);
	assert(unlink(keyboard_path) == 0);
	assert(mkfifo(keyboard_path, 0600) == 0);
	const int read_fd = open(keyboard_path, O_RDONLY | O_NONBLOCK);
	assert(read_fd >= 0);

	HidState hid;
	hid_init(&hid, keyboard_path, "/dev/null", "/dev/null");
	const uint8_t text[] = "aA!\t\b한글";
	assert(hid_type_utf8(&hid, text, sizeof(text) - 1U));

	uint8_t reports[23][8] = { { 0 } };
	for (size_t index = 0; index < sizeof(reports) / sizeof(reports[0]); index++)
	{
		size_t offset = 0;
		while (offset < sizeof(reports[index]))
		{
			const ssize_t result = read(read_fd, reports[index] + offset,
			                            sizeof(reports[index]) - offset);
			assert(result > 0);
			offset += (size_t)result;
		}
	}
	assert(reports[0][0] == 0 && reports[0][2] == 0);
	assert(reports[1][0] == 0 && reports[1][2] == 0x04);
	assert(reports[2][0] == 0 && reports[2][2] == 0);
	assert(reports[3][0] == 0x02 && reports[3][2] == 0x04);
	assert(reports[5][0] == 0x02 && reports[5][2] == 0x1e);
	assert(reports[7][0] == 0 && reports[7][2] == 0x2b);
	assert(reports[9][0] == 0 && reports[9][2] == 0x2a);
	assert(reports[11][0] == 0 && reports[11][2] == 0x0a);
	assert(reports[13][0] == 0 && reports[13][2] == 0x0e);
	assert(reports[15][0] == 0 && reports[15][2] == 0x16);
	assert(reports[17][0] == 0 && reports[17][2] == 0x15);
	assert(reports[19][0] == 0 && reports[19][2] == 0x10);
	assert(reports[21][0] == 0 && reports[21][2] == 0x09);
	assert(reports[22][0] == 0 && reports[22][2] == 0);

	assert(!hid_type_utf8(&hid, (const uint8_t*)"🙂", strlen("🙂")));
	const uint8_t truncated[] = { 0xed, 0xa0, 0x80 };
	assert(!hid_type_utf8(&hid, truncated, sizeof(truncated)));
	assert(close(read_fd) == 0);
	assert(unlink(keyboard_path) == 0);
}

static void test_hid_keyboard_write_recovery(void)
{
	char keyboard_path[] = "/tmp/nanokvm-rdp-keyboard-recovery-XXXXXX";
	const int temp_fd = mkstemp(keyboard_path);
	assert(temp_fd >= 0);
	assert(close(temp_fd) == 0);
	assert(unlink(keyboard_path) == 0);
	assert(mkfifo(keyboard_path, 0600) == 0);

	HidState hid;
	hid_init(&hid, keyboard_path, "/dev/null", "/dev/null");
	assert(!hid_scancode(&hid, 0x1e, false, false));

	const int read_fd = open(keyboard_path, O_RDONLY | O_NONBLOCK);
	assert(read_fd >= 0);
	assert(hid_scancode(&hid, 0x30, false, false));

	uint8_t release_report[8] = { 0 };
	uint8_t key_report[8] = { 0 };
	size_t offset = 0;
	while (offset < sizeof(release_report))
	{
		const ssize_t result = read(read_fd, release_report + offset,
		                            sizeof(release_report) - offset);
		assert(result > 0);
		offset += (size_t)result;
	}
	offset = 0;
	while (offset < sizeof(key_report))
	{
		const ssize_t result = read(read_fd, key_report + offset, sizeof(key_report) - offset);
		assert(result > 0);
		offset += (size_t)result;
	}

	assert(release_report[0] == 0 && release_report[2] == 0);
	assert(key_report[0] == 0 && key_report[2] == 0x05);
	assert(key_report[3] == 0);

	assert(close(read_fd) == 0);
	assert(unlink(keyboard_path) == 0);
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
	const uint8_t text[] = "한";
	assert(protocol_send(sockets[0], NANOKVM_CONTROL_TEXT_UTF8, text, sizeof(text) - 1U));
	assert(protocol_receive(sockets[1], &received));
	assert(received.type == NANOKVM_CONTROL_TEXT_UTF8);
	assert(received.length == sizeof(text) - 1U);
	assert(memcmp(received.payload, text, sizeof(text) - 1U) == 0);
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

static void test_rtp_h264_stap_a(void)
{
	const uint8_t sps[] = { 0x67, 0x42, 0x00, 0x1f };
	const uint8_t pps[] = { 0x68, 0xce, 0x06, 0xe2 };
	uint8_t packet[12 + 1 + 2 + sizeof(sps) + 2 + sizeof(pps)] = { 0 };
	packet[0] = 0x80;
	packet[1] = 0x80 | 96;
	packet[3] = 1;
	packet[5] = 1;
	packet[6] = 0x5f;
	packet[7] = 0x90;
	packet[12] = 24;
	packet[13] = 0;
	packet[14] = sizeof(sps);
	memcpy(packet + 15, sps, sizeof(sps));
	packet[15 + sizeof(sps)] = 0;
	packet[16 + sizeof(sps)] = sizeof(pps);
	memcpy(packet + 17 + sizeof(sps), pps, sizeof(pps));

	RtpH264Reassembler reassembler = { 0 };
	RtpStapTestState state = { 0 };
	rtp_h264_reassembler_init(&reassembler);
	assert(rtp_h264_reassembler_push(&reassembler, packet, sizeof(packet), collect_stap_nal,
	                                 &state, NULL, NULL));
	assert(state.count == 2);
	assert(state.lengths[0] == sizeof(sps));
	assert(state.lengths[1] == sizeof(pps));
	assert(memcmp(state.output[0], sps, sizeof(sps)) == 0);
	assert(memcmp(state.output[1], pps, sizeof(pps)) == 0);
	assert(!state.markers[0]);
	assert(state.markers[1]);
	rtp_h264_reassembler_free(&reassembler);
}

int main(void)
{
	test_h264_annexb();
	test_hid_mapping();
	test_hid_middle_button();
	test_hid_extended_buttons();
	test_hid_text_utf8();
	test_hid_keyboard_write_recovery();
	test_protocol_primitives();
	test_control_wire_message();
	test_rtp_h264_fragmentation_and_loss();
	test_rtp_h264_stap_a();
	return 0;
}
