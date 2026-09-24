#include "h264.h"
#include "hid.h"
#include "protocol.h"
#include "rtp_h264.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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
	char keyboard_path[] = "/tmp/nanokvm-rdp-keyboard-hangul-XXXXXX";
	const int keyboard_fd = mkstemp(keyboard_path);
	assert(keyboard_fd >= 0);
	assert(close(keyboard_fd) == 0);
	assert(unlink(keyboard_path) == 0);
	assert(mkfifo(keyboard_path, 0600) == 0);
	const int keyboard_read = open(keyboard_path, O_RDONLY | O_NONBLOCK);
	assert(keyboard_read >= 0);

	uint8_t usage = 0;
	uint8_t modifier = 0;
	uint8_t mapped_code = 0;
	bool mapped_extended = false;
	assert(hid_translate_scancode(0x1e, false, &usage, &modifier));
	assert(usage == 0x04 && modifier == 0);
	assert(hid_translate_scancode(0x1d, true, &usage, &modifier));
	assert(usage == 0 && modifier == 0x10);
	assert(hid_translate_scancode(0x4b, true, &usage, &modifier));
	assert(usage == 0x50 && modifier == 0);
	hid_map_scancode(0x38, false, true, &mapped_code, &mapped_extended);
	assert(mapped_code == 0x5b && mapped_extended);
	hid_map_scancode(0x38, true, true, &mapped_code, &mapped_extended);
	assert(mapped_code == 0x5c && mapped_extended);
	hid_map_scancode(0x5b, true, true, &mapped_code, &mapped_extended);
	assert(mapped_code == 0x38 && !mapped_extended);
	hid_map_scancode(0x5c, true, true, &mapped_code, &mapped_extended);
	assert(mapped_code == 0x38 && mapped_extended);
	hid_map_scancode(0x38, false, false, &mapped_code, &mapped_extended);
	assert(mapped_code == 0x38 && !mapped_extended);
	hid_map_scancode(0x38, true, false, &mapped_code, &mapped_extended);
	assert(mapped_code == 0x38 && mapped_extended);
	hid_map_scancode(0x5c, true, false, &mapped_code, &mapped_extended);
	assert(mapped_code == 0x5c && mapped_extended);
	assert(hid_translate_scancode(0x3a, false, &usage, &modifier));
	assert(usage == 0x39 && modifier == 0);
	assert(hid_translate_scancode(0x38, true, &usage, &modifier));
	assert(usage == 0 && modifier == 0x40);
	assert(hid_translate_scancode(0x54, false, &usage, &modifier));
	assert(usage == 0x67 && modifier == 0);
	assert(hid_translate_scancode(0x37, true, &usage, &modifier));
	assert(usage == 0x46 && modifier == 0);
	assert(hid_translate_scancode(0x5c, true, &usage, &modifier));
	assert(usage == 0 && modifier == 0x80);
	assert(hid_scale_absolute(0, 1920) == 1);
	assert(hid_scale_absolute(1919, 1920) == 0x7fff);
	assert(hid_clamp_absolute(1200, 1920) == 1200);
	assert(hid_clamp_absolute(3000, 1920) == 1919);
	assert(hid_pointer_flags_from_extended(0x8001U) == 0xc000U);
	assert(hid_pointer_flags_from_extended(0x0001U) == 0x4000U);
	assert(hid_pointer_flags_from_extended(0x8002U) == 0x8000U);

	HidState hid;
	hid_init(&hid, keyboard_path, "/dev/null", "/dev/null");
	uint8_t reports[6][8] = { { 0 } };
	const struct
	{
		uint8_t code;
		bool extended;
		uint8_t modifier;
		uint8_t usage;
	} presses[] = {
		{ 0x38, true, 0x40, 0 },
		{ 0x3a, false, 0, 0x39 },
		{ 0x5c, true, 0x80, 0 },
	};
	for (size_t press = 0; press < 3; press++)
	{
		assert(hid_scancode(&hid, presses[press].code, presses[press].extended, false));
		assert(hid_scancode(&hid, presses[press].code, presses[press].extended, true));
		for (size_t report_index = 0; report_index < 2; report_index++)
		{
			uint8_t* report = reports[press * 2U + report_index];
			size_t offset = 0;
			while (offset < 8)
			{
				const ssize_t result = read(keyboard_read, report + offset, 8U - offset);
				assert(result > 0);
				offset += (size_t)result;
			}
		}
		assert(!hid_keyboard_pending(&hid));
		assert(hid.modifiers == 0);
		assert(reports[press * 2U][0] == presses[press].modifier);
		assert(reports[press * 2U][2] == presses[press].usage);
		assert(reports[press * 2U + 1U][0] == 0);
		assert(reports[press * 2U + 1U][2] == 0);
	}
	assert(close(keyboard_read) == 0);
	assert(unlink(keyboard_path) == 0);
}

static void test_hid_control_space_passthrough(void)
{
	HidState hid;
	hid_init(&hid, "/dev/null", "/dev/null", "/dev/null");
	assert(hid_scancode(&hid, 0x1d, false, false));
	assert(hid.modifiers == 0x01);
	assert(hid_scancode(&hid, 0x39, false, false));
	assert(hid.modifiers == 0x01);
	assert(hid.usages[0x2c]);
	assert(!hid_keyboard_pending(&hid));
	assert(hid_scancode(&hid, 0x39, false, true));
	assert(!hid.usages[0x2c]);
	assert(hid.modifiers == 0x01);
	assert(hid_scancode(&hid, 0x1d, false, true));
	assert(hid.modifiers == 0);
	assert(hid_scancode(&hid, 0x20, false, false));
	assert(hid.usages[0x07]);
	assert(hid_scancode(&hid, 0x20, false, true));
	assert(!hid.usages[0x07]);
	assert(hid.modifiers == 0);
	assert(!hid_keyboard_pending(&hid));
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
	uint8_t reports[3][6] = { { 0 } };
	assert(read(read_fd, reports, sizeof(reports)) == (ssize_t)sizeof(reports));
	memcpy(report, reports[2], sizeof(report));
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
	assert(report[0] == 0x00 && report[5] == 0);
	assert(close(read_fd) == 0);

	assert(hid_wheel(&hid, 0x0278U));
	read_fd = open(touch_path, O_RDONLY);
	assert(read_fd >= 0);
	uint8_t wheel_report[18] = { 0 };
	assert(read(read_fd, wheel_report, sizeof(wheel_report)) == (ssize_t)sizeof(wheel_report));
	assert(wheel_report[6] == 0x00 && wheel_report[11] == 1);
	assert(wheel_report[17] == 0);
	assert(close(read_fd) == 0);

	assert(hid_absolute(&hid, 100, 200, 1920, 1080, 0));
	read_fd = open(touch_path, O_RDONLY);
	assert(read_fd >= 0);
	memset(report, 0, sizeof(report));
	assert(read(read_fd, report, sizeof(report)) == (ssize_t)sizeof(report));
	assert(report[0] == 0x00 && report[5] == 0);
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
	assert(report[0] == 0x00 && report[5] == 0);
	assert(close(read_fd) == 0);
	assert(unlink(mouse_path) == 0);
	assert(unlink(touch_path) == 0);
}

static void test_hid_horizontal_wheel(void)
{
	char touch_path[] = "/tmp/nanokvm-rdp-touch-hwheel-XXXXXX";
	const int touch_fd = mkstemp(touch_path);
	assert(touch_fd >= 0);
	assert(close(touch_fd) == 0);
	assert(setenv("NANOKVM_HID_ABSOLUTE_REPORT_LENGTH", "7", 1) == 0);

	HidState hid;
	hid_init(&hid, "/dev/null", "/dev/null", touch_path);
	assert(hid.absolute_report_length == 7);
	hid.last_x = 0x1111;
	hid.last_y = 0x2222;
	assert(hid_wheel(&hid, 0x0478U));
	int read_fd = open(touch_path, O_RDONLY);
	assert(read_fd >= 0);
	uint8_t report[14] = { 0 };
	ssize_t got = read(read_fd, report, sizeof(report));
	assert(got == 14);
	assert(report[0] == 0 && report[1] == 0x11 && report[2] == 0x11 &&
	       report[3] == 0x22 && report[4] == 0x22 && report[5] == 0 && report[6] == 1);
	assert(report[7] == 0 && report[12] == 0 && report[13] == 0);
	assert(close(read_fd) == 0);

	assert(hid_wheel(&hid, 0x0578U));
	read_fd = open(touch_path, O_RDONLY);
	assert(read_fd >= 0);
	uint8_t second[28] = { 0 };
	got = read(read_fd, second, sizeof(second));
	assert(got == (ssize_t)sizeof(second));
	assert(second[14] == 0 && second[19] == 0 && second[20] == 0xff);
	assert(second[26] == 0 && second[27] == 0);
	assert(close(read_fd) == 0);
	assert(unlink(touch_path) == 0);
	assert(unsetenv("NANOKVM_HID_ABSOLUTE_REPORT_LENGTH") == 0);
}

static void test_hid_release_all_absolute_length(void)
{
	char touch_template[] = "/tmp/nanokvm-rdp-touch-release-XXXXXX";
	const int touch_temp = mkstemp(touch_template);
	assert(touch_temp >= 0);
	assert(close(touch_temp) == 0);
	assert(unlink(touch_template) == 0);
	assert(mkfifo(touch_template, 0600) == 0);
	const int touch_read = open(touch_template, O_RDONLY | O_NONBLOCK);
	assert(touch_read >= 0);
	assert(setenv("NANOKVM_HID_ABSOLUTE_REPORT_LENGTH", "7", 1) == 0);

	char keyboard_template[] = "/tmp/nanokvm-rdp-keyboard-release-XXXXXX";
	const int keyboard_temp = mkstemp(keyboard_template);
	assert(keyboard_temp >= 0);
	assert(close(keyboard_temp) == 0);
	assert(unlink(keyboard_template) == 0);
	assert(mkfifo(keyboard_template, 0600) == 0);
	const int keyboard_read = open(keyboard_template, O_RDONLY | O_NONBLOCK);
	assert(keyboard_read >= 0);

	HidState hid;
	hid_init(&hid, keyboard_template, "/dev/null", touch_template);
	hid.last_x = 0x1234;
	hid.last_y = 0x5678;
	hid.buttons = 0x02;
	hid.mouse_buttons = 0x02;
	hid_release_all(&hid);

	uint8_t report[7] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	assert(read(touch_read, report, sizeof(report)) == (ssize_t)sizeof(report));
	assert(report[0] == 0);
	assert(report[1] == 0x34 && report[2] == 0x12);
	assert(report[3] == 0x78 && report[4] == 0x56);
	assert(report[5] == 0 && report[6] == 0);
	uint8_t extra = 0xff;
	assert(read(touch_read, &extra, 1) == -1);
	assert(hid.buttons == 0 && hid.mouse_buttons == 0);
	uint8_t keyboard[8] = { 0xff };
	assert(read(keyboard_read, keyboard, sizeof(keyboard)) == (ssize_t)sizeof(keyboard));
	assert(keyboard[0] == 0 && keyboard[2] == 0);
	assert(close(keyboard_read) == 0);
	assert(unlink(keyboard_template) == 0);
	assert(close(touch_read) == 0);
	assert(unlink(touch_template) == 0);
	assert(unsetenv("NANOKVM_HID_ABSOLUTE_REPORT_LENGTH") == 0);
}

static void test_hid_right_button_release(void)
{
	char mouse_path[] = "/tmp/nanokvm-rdp-mouse-right-XXXXXX";
	char touch_path[] = "/tmp/nanokvm-rdp-touch-right-XXXXXX";
	const int mouse_fd = mkstemp(mouse_path);
	const int touch_fd = mkstemp(touch_path);
	assert(mouse_fd >= 0 && touch_fd >= 0);
	assert(close(mouse_fd) == 0);
	assert(close(touch_fd) == 0);
	assert(setenv("NANOKVM_HID_ABSOLUTE_REPORT_LENGTH", "7", 1) == 0);
	assert(unlink(mouse_path) == 0);
	assert(unlink(touch_path) == 0);
	assert(mkfifo(mouse_path, 0600) == 0);
	assert(mkfifo(touch_path, 0600) == 0);
	const int mouse_read = open(mouse_path, O_RDONLY | O_NONBLOCK);
	const int touch_read = open(touch_path, O_RDONLY | O_NONBLOCK);
	assert(mouse_read >= 0 && touch_read >= 0);

	HidState hid;
	hid_init(&hid, "/dev/null", mouse_path, touch_path);
	assert(hid.absolute_report_length == 7);

	assert(hid_absolute(&hid, 100, 200, 1920, 1080, 0xa000U));
	assert(hid.buttons == 0x02 && hid.mouse_buttons == 0x02);
	uint8_t touch[7] = { 0 };
	uint8_t mouse[4] = { 0 };
	assert(read(touch_read, touch, sizeof(touch)) == (ssize_t)sizeof(touch));
	assert(touch[0] == 0x02);
	assert(read(mouse_read, mouse, sizeof(mouse)) == (ssize_t)sizeof(mouse));
	assert(mouse[0] == 0x02);
	assert(hid_relative(&hid, 0, 0, 0));
	assert(hid.buttons == 0 && hid.mouse_buttons == 0);
	memset(touch, 0xff, sizeof(touch));
	assert(read(touch_read, touch, sizeof(touch)) == (ssize_t)sizeof(touch));
	assert(touch[0] == 0);
	assert(read(mouse_read, mouse, sizeof(mouse)) == (ssize_t)sizeof(mouse));
	assert(mouse[0] == 0);

	assert(hid_relative(&hid, 3, -2, 0x02));
	assert(hid.buttons == 0x02 && hid.mouse_buttons == 0x02);
	assert(read(mouse_read, mouse, sizeof(mouse)) == (ssize_t)sizeof(mouse));
	assert(mouse[0] == 0x02);
	memset(touch, 0xff, sizeof(touch));
	assert(read(touch_read, touch, sizeof(touch)) == (ssize_t)sizeof(touch));
	assert(touch[0] == 0x02);
	assert(hid_absolute(&hid, 120, 220, 1920, 1080, 0x2000U));
	assert(hid.buttons == 0 && hid.mouse_buttons == 0);
	memset(touch, 0xff, sizeof(touch));
	assert(read(touch_read, touch, sizeof(touch)) == (ssize_t)sizeof(touch));
	assert(touch[0] == 0);
	assert(read(mouse_read, mouse, sizeof(mouse)) == (ssize_t)sizeof(mouse));
	assert(mouse[0] == 0);
	assert(read(mouse_read, mouse, sizeof(mouse)) == (ssize_t)sizeof(mouse));
	assert(mouse[0] == 0);

	assert(hid_absolute(&hid, 100, 200, 1920, 1080, 0xa000U));
	assert(read(touch_read, touch, sizeof(touch)) == (ssize_t)sizeof(touch));
	assert(touch[0] == 0x02);
	assert(read(mouse_read, mouse, sizeof(mouse)) == (ssize_t)sizeof(mouse));
	assert(mouse[0] == 0x02);
	hid_release_all(&hid);
	assert(hid.buttons == 0 && hid.mouse_buttons == 0 && hid.wheel == 0 && hid.pan == 0);
	memset(touch, 0xff, sizeof(touch));
	assert(read(touch_read, touch, sizeof(touch)) == (ssize_t)sizeof(touch));
	assert(touch[0] == 0 && touch[6] == 0);
	assert(read(mouse_read, mouse, sizeof(mouse)) == (ssize_t)sizeof(mouse));
	assert(mouse[0] == 0);
	assert(close(mouse_read) == 0);
	assert(close(touch_read) == 0);

	assert(unlink(mouse_path) == 0);
	assert(unlink(touch_path) == 0);
	assert(unsetenv("NANOKVM_HID_ABSOLUTE_REPORT_LENGTH") == 0);
}

static void test_hid_stuck_right_button_times_out(void)
{
	char mouse_path[] = "/tmp/nanokvm-rdp-mouse-stuck-XXXXXX";
	char touch_path[] = "/tmp/nanokvm-rdp-touch-stuck-XXXXXX";
	const int mouse_fd = mkstemp(mouse_path);
	const int touch_fd = mkstemp(touch_path);
	assert(mouse_fd >= 0 && touch_fd >= 0);
	assert(close(mouse_fd) == 0);
	assert(close(touch_fd) == 0);
	assert(unlink(mouse_path) == 0);
	assert(unlink(touch_path) == 0);
	assert(mkfifo(mouse_path, 0600) == 0);
	assert(mkfifo(touch_path, 0600) == 0);
	const int mouse_read = open(mouse_path, O_RDONLY | O_NONBLOCK);
	const int touch_read = open(touch_path, O_RDONLY | O_NONBLOCK);
	assert(mouse_read >= 0 && touch_read >= 0);

	HidState hid;
	hid_init(&hid, "/dev/null", mouse_path, touch_path);
	assert(hid_absolute(&hid, 100, 200, 1920, 1080, 0xa000U));
	assert(hid.buttons == 0x02);
	uint8_t ignored[16];
	while (read(mouse_read, ignored, sizeof(ignored)) > 0)
		;
	while (read(touch_read, ignored, sizeof(ignored)) > 0)
		;
	hid_release_stuck_buttons(&hid, hid.buttons_changed_at + HID_BUTTON_STUCK_TIMEOUT_MS - 1U);
	assert(hid.buttons == 0x02);
	assert(read(mouse_read, ignored, sizeof(ignored)) < 0);
	hid_release_stuck_buttons(&hid, hid.buttons_changed_at + HID_BUTTON_STUCK_TIMEOUT_MS);
	assert(hid.buttons == 0 && hid.mouse_buttons == 0);
	uint8_t mouse[4] = { 0xff };
	uint8_t touch[7] = { 0xff };
	assert(read(mouse_read, mouse, sizeof(mouse)) == (ssize_t)sizeof(mouse));
	assert(mouse[0] == 0);
	assert(read(mouse_read, mouse, sizeof(mouse)) == (ssize_t)sizeof(mouse));
	assert(mouse[0] == 0);
	assert(read(touch_read, touch, sizeof(touch)) == 6);
	assert(touch[0] == 0);
	assert(close(mouse_read) == 0);
	assert(close(touch_read) == 0);
	assert(unlink(mouse_path) == 0);
	assert(unlink(touch_path) == 0);
}

static void test_hid_right_button_release_six_byte(void)
{
	char mouse_path[] = "/tmp/nanokvm-rdp-mouse-right6-XXXXXX";
	char touch_path[] = "/tmp/nanokvm-rdp-touch-right6-XXXXXX";
	const int mouse_fd = mkstemp(mouse_path);
	const int touch_fd = mkstemp(touch_path);
	assert(mouse_fd >= 0 && touch_fd >= 0);
	assert(close(mouse_fd) == 0);
	assert(close(touch_fd) == 0);
	assert(unlink(mouse_path) == 0);
	assert(unlink(touch_path) == 0);
	assert(mkfifo(mouse_path, 0600) == 0);
	assert(mkfifo(touch_path, 0600) == 0);
	const int mouse_read = open(mouse_path, O_RDONLY | O_NONBLOCK);
	const int touch_read = open(touch_path, O_RDONLY | O_NONBLOCK);
	assert(mouse_read >= 0 && touch_read >= 0);

	HidState hid;
	hid_init(&hid, "/dev/null", mouse_path, touch_path);
	assert(hid.absolute_report_length == 6);

	assert(hid_absolute(&hid, 100, 200, 1920, 1080, 0xa000U));
	uint8_t touch[6] = { 0 };
	uint8_t mouse[4] = { 0 };
	assert(read(touch_read, touch, sizeof(touch)) == (ssize_t)sizeof(touch));
	assert(touch[0] == 0x02 && touch[5] == 0);
	assert(read(mouse_read, mouse, sizeof(mouse)) == (ssize_t)sizeof(mouse));
	assert(mouse[0] == 0x02 && mouse[1] == 0 && mouse[2] == 0);

	assert(hid_absolute(&hid, 100, 200, 1920, 1080, 0x2000U));
	assert(hid.buttons == 0 && hid.mouse_buttons == 0);
	memset(touch, 0xff, sizeof(touch));
	assert(read(touch_read, touch, sizeof(touch)) == (ssize_t)sizeof(touch));
	assert(touch[0] == 0 && touch[5] == 0);
	assert(read(mouse_read, mouse, sizeof(mouse)) == (ssize_t)sizeof(mouse));
	assert(mouse[0] == 0);
	assert(read(mouse_read, mouse, sizeof(mouse)) == (ssize_t)sizeof(mouse));
	assert(mouse[0] == 0 && mouse[1] == 0 && mouse[2] == 0 && mouse[3] == 0);

	uint8_t extra = 0xff;
	assert(read(touch_read, &extra, 1) <= 0);
	assert(read(mouse_read, &extra, 1) <= 0);
	assert(close(mouse_read) == 0);
	assert(close(touch_read) == 0);
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

	const int read_fd = open(keyboard_path, O_RDONLY | O_NONBLOCK);
	assert(read_fd >= 0);

	HidState hid;
	hid_init(&hid, keyboard_path, "/dev/null", "/dev/null");
	assert(hid.keyboard_fd >= 0);
	assert(close(hid.keyboard_fd) == 0);
	assert(close(read_fd) == 0);
	hid.keyboard_fd = -1;
	assert(!hid_scancode(&hid, 0x1e, false, false));
	assert(hid.keyboard_fd < 0);
	assert(hid.usages[0x04] == true);

	const int recovered_read = open(keyboard_path, O_RDONLY | O_NONBLOCK);
	assert(recovered_read >= 0);
	assert(hid_scancode(&hid, 0x30, false, false));
	assert(hid.keyboard_desynced == true);
	hid_keyboard_flush(&hid);
	assert(hid.keyboard_desynced == false);
	assert(hid.usages[0x04] == true);
	assert(hid.usages[0x05] == true);
	uint8_t key_report[8] = { 0 };
	size_t offset = 0;
	while (offset < sizeof(key_report))
	{
		const ssize_t result = read(recovered_read, key_report + offset, sizeof(key_report) - offset);
		assert(result > 0);
		offset += (size_t)result;
	}
	/* 리더가 생기기 전 실패는 키를 지우지 않는다. 다음 성공 보고는 그때의 상태다. */
	assert(key_report[0] == 0 && key_report[2] == 0x04 && key_report[3] == 0x05);
	assert(hid.keyboard_desynced == false);
	assert(hid.usages[0x04] == true);
	assert(hid.usages[0x05] == true);

	assert(close(hid.keyboard_fd) == 0);
	hid.keyboard_fd = open("/dev/full", O_WRONLY | O_CLOEXEC | O_NONBLOCK);
	if (hid.keyboard_fd >= 0)
	{
		assert(hid_scancode(&hid, 0x1e, false, true));
		assert(hid.keyboard_desynced == true);
		assert(hid.usages[0x04] == false);
		assert(hid.usages[0x05] == true);
		assert(close(hid.keyboard_fd) == 0);
	}
	assert(close(recovered_read) == 0);
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


static void test_hid_emoji_paste(void)
{
	char keyboard_path[] = "/tmp/nanokvm-rdp-keyboard-paste-XXXXXX";
	const int temp_fd = mkstemp(keyboard_path);
	assert(temp_fd >= 0);
	assert(close(temp_fd) == 0);
	assert(unlink(keyboard_path) == 0);
	assert(mkfifo(keyboard_path, 0600) == 0);
	char paste_path[160] = { 0 };
	assert(snprintf(paste_path, sizeof(paste_path), "%s.paste", keyboard_path) > 0);
	assert(mkfifo(paste_path, 0600) == 0);
	const int keyboard_read = open(keyboard_path, O_RDONLY | O_NONBLOCK);
	const int paste_read = open(paste_path, O_RDONLY | O_NONBLOCK);
	assert(keyboard_read >= 0);
	assert(paste_read >= 0);

	HidState hid;
	hid_init(&hid, keyboard_path, "/dev/null", "/dev/null");
	assert(hid.paste_fd >= 0);
	const uint8_t emoji[] = "🙂";
	assert(hid_type_utf8(&hid, emoji, sizeof(emoji) - 1U));

	char pasted[16] = { 0 };
	size_t pasted_length = 0;
	while (pasted_length + 1U < sizeof(pasted))
	{
		const ssize_t result = read(paste_read, pasted + pasted_length,
		                            sizeof(pasted) - 1U - pasted_length);
		if (result < 0 && errno == EAGAIN)
			break;
		assert(result > 0);
		pasted_length += (size_t)result;
		if (pasted[pasted_length - 1U] == '\n')
			break;
	}
	assert(strcmp(pasted, "'🙂'\n") == 0);
	uint8_t down[8] = { 0 };
	uint8_t up[8] = { 0 };
	size_t offset = 0;
	while (offset < 8)
	{
		const ssize_t result = read(keyboard_read, down + offset, 8U - offset);
		assert(result > 0);
		offset += (size_t)result;
	}
	offset = 0;
	while (offset < 8)
	{
		const ssize_t result = read(keyboard_read, up + offset, 8U - offset);
		assert(result > 0);
		offset += (size_t)result;
	}
	assert(down[0] == 0 && down[2] == 0);
	assert(up[0] == 0x08 && up[2] == 0x19);
	assert(close(keyboard_read) == 0);
	assert(close(paste_read) == 0);
	assert(unlink(keyboard_path) == 0);
	assert(unlink(paste_path) == 0);
}

int main(void)
{
	test_h264_annexb();
	test_hid_mapping();
	test_hid_control_space_passthrough();
	test_hid_middle_button();
	test_hid_extended_buttons();
	test_hid_horizontal_wheel();
	test_hid_release_all_absolute_length();
	test_hid_right_button_release();
	test_hid_right_button_release_six_byte();
	test_hid_stuck_right_button_times_out();
	test_hid_text_utf8();
	test_hid_emoji_paste();
	test_hid_keyboard_write_recovery();
	test_protocol_primitives();
	test_control_wire_message();
	test_rtp_h264_fragmentation_and_loss();
	test_rtp_h264_stap_a();
	return 0;
}
