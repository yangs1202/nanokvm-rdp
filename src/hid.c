#include "hid.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define KBD_FLAGS_RELEASE 0x8000
#define PTR_FLAGS_HWHEEL 0x0400
#define PTR_FLAGS_WHEEL 0x0200
#define PTR_FLAGS_WHEEL_NEGATIVE 0x0100
#define PTR_FLAGS_DOWN 0x8000
#define PTR_FLAGS_BUTTON1 0x1000
#define PTR_FLAGS_BUTTON2 0x2000
#define PTR_FLAGS_BUTTON3 0x4000
#define WHEEL_ROTATION_MASK 0x01FF

static bool write_report(const char* path, const uint8_t* report, size_t length)
{
	const int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	const ssize_t written = write(fd, report, length);
	const int saved_errno = errno;
	(void)close(fd);
	errno = saved_errno;
	return written == (ssize_t)length;
}

void hid_init(HidState* hid, const char* keyboard, const char* mouse, const char* touch)
{
	memset(hid, 0, sizeof(*hid));
	(void)snprintf(hid->keyboard_path, sizeof(hid->keyboard_path), "%s",
	               keyboard ? keyboard : "/dev/hidg0");
	(void)snprintf(hid->mouse_path, sizeof(hid->mouse_path), "%s",
	               mouse ? mouse : "/dev/hidg1");
	(void)snprintf(hid->touch_path, sizeof(hid->touch_path), "%s",
	               touch ? touch : "/dev/hidg2");
	hid->last_x = 0x3fff;
	hid->last_y = 0x3fff;
}

uint16_t hid_scale_absolute(uint16_t value, uint32_t dimension)
{
	if (dimension <= 1)
		return 0x3fff;
	uint32_t scaled = ((uint32_t)value * 0x7fffU) / (dimension - 1U);
	if (scaled < 1U)
		scaled = 1U;
	if (scaled > 0x7fffU)
		scaled = 0x7fffU;
	return (uint16_t)scaled;
}

static bool send_keyboard(HidState* hid)
{
	uint8_t report[8] = { 0 };
	report[0] = hid->modifiers;
	size_t index = 2;
	for (unsigned usage = 0; usage < 256 && index < sizeof(report); usage++)
	{
		if (hid->usages[usage])
			report[index++] = (uint8_t)usage;
	}
	return write_report(hid->keyboard_path, report, sizeof(report));
}

bool hid_translate_scancode(uint8_t code, bool extended, uint8_t* usage, uint8_t* modifier)
{
	static const uint8_t normal[128] = {
		[0x01] = 0x29, [0x02] = 0x1e, [0x03] = 0x1f, [0x04] = 0x20,
		[0x05] = 0x21, [0x06] = 0x22, [0x07] = 0x23, [0x08] = 0x24,
		[0x09] = 0x25, [0x0a] = 0x26, [0x0b] = 0x27, [0x0c] = 0x2d,
		[0x0d] = 0x2e, [0x0e] = 0x2a, [0x0f] = 0x2b, [0x10] = 0x14,
		[0x11] = 0x1a, [0x12] = 0x08, [0x13] = 0x15, [0x14] = 0x17,
		[0x15] = 0x1c, [0x16] = 0x18, [0x17] = 0x0c, [0x18] = 0x12,
		[0x19] = 0x13, [0x1a] = 0x2f, [0x1b] = 0x30, [0x1c] = 0x28,
		[0x1e] = 0x04, [0x1f] = 0x16, [0x20] = 0x07, [0x21] = 0x09,
		[0x22] = 0x0a, [0x23] = 0x0b, [0x24] = 0x0d, [0x25] = 0x0e,
		[0x26] = 0x0f, [0x27] = 0x33, [0x28] = 0x34, [0x29] = 0x35,
		[0x2b] = 0x31, [0x2c] = 0x1d, [0x2d] = 0x1b, [0x2e] = 0x06,
		[0x2f] = 0x19, [0x30] = 0x05, [0x31] = 0x11, [0x32] = 0x10,
		[0x33] = 0x36, [0x34] = 0x37, [0x35] = 0x38, [0x37] = 0x55,
		[0x39] = 0x2c, [0x3a] = 0x39, [0x3b] = 0x3a, [0x3c] = 0x3b,
		[0x3d] = 0x3c, [0x3e] = 0x3d, [0x3f] = 0x3e, [0x40] = 0x3f,
		[0x41] = 0x40, [0x42] = 0x41, [0x43] = 0x42, [0x44] = 0x43,
		[0x45] = 0x53, [0x46] = 0x47, [0x47] = 0x5f, [0x48] = 0x60,
		[0x49] = 0x61, [0x4a] = 0x56, [0x4b] = 0x5c, [0x4c] = 0x5d,
		[0x4d] = 0x5e, [0x4e] = 0x57, [0x4f] = 0x59, [0x50] = 0x5a,
		[0x51] = 0x5b, [0x52] = 0x62, [0x53] = 0x63, [0x56] = 0x64,
		[0x57] = 0x44, [0x58] = 0x45,
	};

	*usage = 0;
	*modifier = 0;
	if (extended)
	{
		switch (code)
		{
			case 0x1c: *usage = 0x58; return true;
			case 0x1d: *modifier = 0x10; return true;
			case 0x35: *usage = 0x54; return true;
			case 0x38: *modifier = 0x40; return true;
			case 0x47: *usage = 0x4a; return true;
			case 0x48: *usage = 0x52; return true;
			case 0x49: *usage = 0x4b; return true;
			case 0x4b: *usage = 0x50; return true;
			case 0x4d: *usage = 0x4f; return true;
			case 0x4f: *usage = 0x4d; return true;
			case 0x50: *usage = 0x51; return true;
			case 0x51: *usage = 0x4e; return true;
			case 0x52: *usage = 0x49; return true;
			case 0x53: *usage = 0x4c; return true;
			case 0x5b: *modifier = 0x08; return true;
			case 0x5c: *modifier = 0x80; return true;
			case 0x5d: *usage = 0x65; return true;
			default: return false;
		}
	}

	switch (code)
	{
		case 0x1d: *modifier = 0x01; return true;
		case 0x2a: *modifier = 0x02; return true;
		case 0x36: *modifier = 0x20; return true;
		case 0x38: *modifier = 0x04; return true;
		default: break;
	}
	if (code >= sizeof(normal) || normal[code] == 0)
		return false;
	*usage = normal[code];
	return true;
}

bool hid_scancode(HidState* hid, uint8_t code, bool extended, bool release)
{
	uint8_t usage = 0;
	uint8_t modifier = 0;
	if (!hid_translate_scancode(code, extended, &usage, &modifier))
		return true;
	if (modifier != 0)
	{
		if (release)
			hid->modifiers &= (uint8_t)~modifier;
		else
			hid->modifiers |= modifier;
	}
	else
		hid->usages[usage] = !release;
	return send_keyboard(hid);
}

static uint8_t touch_buttons(uint8_t buttons)
{
	uint8_t report = 0;
	if ((buttons & 0x01U) != 0)
		report |= 0x01;
	if ((buttons & 0x02U) != 0)
		report |= 0x10;
	return report;
}

bool hid_absolute(HidState* hid, uint16_t x, uint16_t y, uint32_t width, uint32_t height,
	              uint16_t flags)
{
	if ((flags & PTR_FLAGS_BUTTON1) != 0)
	{
		if ((flags & PTR_FLAGS_DOWN) != 0)
			hid->buttons |= 0x01;
		else
			hid->buttons &= (uint8_t)~0x01U;
	}
	if ((flags & PTR_FLAGS_BUTTON2) != 0)
	{
		if ((flags & PTR_FLAGS_DOWN) != 0)
			hid->buttons |= 0x02;
		else
			hid->buttons &= (uint8_t)~0x02U;
	}
	if ((flags & PTR_FLAGS_BUTTON3) != 0)
	{
		if ((flags & PTR_FLAGS_DOWN) != 0)
			hid->buttons |= 0x04;
		else
			hid->buttons &= (uint8_t)~0x04U;
	}

	hid->last_x = hid_scale_absolute(x, width);
	hid->last_y = hid_scale_absolute(y, height);
	uint8_t report[6] = { touch_buttons(hid->buttons), (uint8_t)(hid->last_x & 0xffU),
		(uint8_t)(hid->last_x >> 8U), (uint8_t)(hid->last_y & 0xffU),
		(uint8_t)(hid->last_y >> 8U), 0 };
	return write_report(hid->touch_path, report, sizeof(report));
}

bool hid_wheel(HidState* hid, uint16_t flags)
{
	if ((flags & (PTR_FLAGS_WHEEL | PTR_FLAGS_HWHEEL)) == 0)
		return true;
	int delta = (int)(flags & WHEEL_ROTATION_MASK);
	if ((flags & PTR_FLAGS_WHEEL_NEGATIVE) != 0)
		delta = -delta;
	int detents = delta / 120;
	if (detents == 0 && delta != 0)
		detents = delta > 0 ? 1 : -1;
	if (detents > 127)
		detents = 127;
	if (detents < -127)
		detents = -127;
	const uint8_t report[4] = { 0, 0, 0, (uint8_t)(int8_t)detents };
	return write_report(hid->mouse_path, report, sizeof(report));
}

void hid_release_all(HidState* hid)
{
	memset(hid->usages, 0, sizeof(hid->usages));
	hid->modifiers = 0;
	hid->buttons = 0;
	const uint8_t keyboard[8] = { 0 };
	const uint8_t mouse[4] = { 0 };
	const uint8_t touch[6] = { 0, (uint8_t)(hid->last_x & 0xffU),
		(uint8_t)(hid->last_x >> 8U), (uint8_t)(hid->last_y & 0xffU),
		(uint8_t)(hid->last_y >> 8U), 0 };
	(void)write_report(hid->keyboard_path, keyboard, sizeof(keyboard));
	(void)write_report(hid->mouse_path, mouse, sizeof(mouse));
	(void)write_report(hid->touch_path, touch, sizeof(touch));
}
