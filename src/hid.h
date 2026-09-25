#ifndef NANOKVM_RDP_HID_H
#define NANOKVM_RDP_HID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HID_REPORT_QUEUE_CAPACITY 256U
#define HID_REPORT_STALL_TIMEOUT_MS 1000U

typedef struct
{
	uint8_t data[8];
	uint8_t endpoint;
	uint8_t length;
	uint16_t delay_ms;
	bool motion;
	uint32_t paste_codepoint;
	uint64_t queued_at;
} HidReport;

typedef struct
{
	HidReport reports[HID_REPORT_QUEUE_CAPACITY];
	unsigned head;
	unsigned count;
	uint64_t ready_at;
	uint64_t blocked_at;
	uint8_t inflight_endpoint;
} HidReportQueue;

typedef struct
{
	uint8_t modifiers;
	bool usages[256];
	HidReportQueue keyboard_queue;
	HidReportQueue pointer_queue;
	uint16_t keyboard_delay_ms;
	uint32_t keyboard_paste_codepoint;
	uint64_t reports_sent;
	uint64_t write_retries;
	uint64_t write_errors;
	uint64_t queue_overflows;
	uint64_t max_queue_age_ms;
	uint64_t feedback_reports;
	uint64_t feedback_errors;
	uint64_t feedback_retry_at;
	uint8_t keyboard_leds;
	uint8_t submitted_state[3]; /* Modifier/button byte last submitted per USB endpoint. */
	uint16_t last_x;
	uint16_t last_y;
	uint8_t buttons;
	uint8_t mouse_buttons;
	bool keyboard_desynced;
	int8_t wheel;
	int8_t pan;
	uint8_t absolute_report_length;
	int keyboard_fd;
	int keyboard_feedback_fd;
	int mouse_fd;
	int touch_fd;
	int paste_fd;
	char keyboard_path[128];
	char mouse_path[128];
	char touch_path[128];
	char paste_path[160];
} HidState;

void hid_init(HidState* hid, const char* keyboard, const char* mouse, const char* touch);
bool hid_scancode(HidState* hid, uint8_t code, bool extended, bool release);
bool hid_keyboard_pending(const HidState* hid);
void hid_keyboard_flush(HidState* hid);
bool hid_type_utf8(HidState* hid, const uint8_t* text, size_t length);
bool hid_absolute(HidState* hid, uint16_t x, uint16_t y, uint32_t width, uint32_t height,
	              uint16_t flags);
bool hid_relative(HidState* hid, int16_t x, int16_t y, uint8_t buttons);
bool hid_wheel(HidState* hid, uint16_t flags);
/* Pump every input endpoint even when no new network input arrives. */
bool hid_flush(HidState* hid);
bool hid_pending(const HidState* hid);
int hid_poll_timeout(const HidState* hid, int idle_ms);
struct pollfd;
size_t hid_pollfds(const HidState* hid, struct pollfd* fds);
void hid_release_all(HidState* hid);
/* Ordered protocol resynchronization; preserve input already queued for USB. */
bool hid_synchronize(HidState* hid);

uint16_t hid_scale_absolute(uint16_t value, uint32_t dimension);
uint16_t hid_clamp_absolute(uint16_t value, uint16_t dimension);
uint8_t hid_pointer_buttons(uint8_t buttons, uint16_t flags);
uint16_t hid_pointer_flags_from_extended(uint16_t flags);
void hid_map_scancode(uint8_t code, bool extended, bool swap_alt_command,
                      uint8_t* mapped_code, bool* mapped_extended);
bool hid_translate_scancode(uint8_t code, bool extended, uint8_t* usage, uint8_t* modifier);

#endif
