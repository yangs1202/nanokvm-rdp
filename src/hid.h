#ifndef NANOKVM_RDP_HID_H
#define NANOKVM_RDP_HID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
	uint8_t modifiers;
	bool usages[256];
	uint16_t last_x;
	uint16_t last_y;
	uint8_t buttons;
	uint8_t mouse_buttons;
	char keyboard_path[128];
	char mouse_path[128];
	char touch_path[128];
} HidState;

void hid_init(HidState* hid, const char* keyboard, const char* mouse, const char* touch);
bool hid_scancode(HidState* hid, uint8_t code, bool extended, bool release);
bool hid_type_utf8(HidState* hid, const uint8_t* text, size_t length);
bool hid_absolute(HidState* hid, uint16_t x, uint16_t y, uint32_t width, uint32_t height,
	              uint16_t flags);
bool hid_relative(HidState* hid, int16_t x, int16_t y, uint8_t buttons);
bool hid_wheel(HidState* hid, uint16_t flags);
void hid_release_all(HidState* hid);

uint16_t hid_scale_absolute(uint16_t value, uint32_t dimension);
uint16_t hid_clamp_absolute(uint16_t value, uint16_t dimension);
bool hid_translate_scancode(uint8_t code, bool extended, uint8_t* usage, uint8_t* modifier);

#endif
