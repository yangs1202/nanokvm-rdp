#include "hid.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define KBD_FLAGS_RELEASE 0x8000
#define PTR_FLAGS_WHEEL 0x0200
#define PTR_FLAGS_WHEEL_NEGATIVE 0x0100
#define PTR_FLAGS_DOWN 0x8000
#define PTR_FLAGS_BUTTON1 0x1000
#define PTR_FLAGS_BUTTON2 0x2000
#define PTR_FLAGS_BUTTON3 0x4000
#define PTR_XFLAGS_BUTTON1 0x0001
#define PTR_XFLAGS_BUTTON2 0x0002
#define WHEEL_ROTATION_MASK 0x01FF
#define HID_MOUSE_BUTTONS_MASK 0x07
#define HID_TOUCH_BUTTONS_MASK 0x1F
#define HID_TOUCH_XBUTTON1 0x08
#define HID_TOUCH_XBUTTON2 0x10

static bool write_report(const char* path, const uint8_t* report, size_t length)
{
	const int fd = open(path, O_WRONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0)
		return false;
	ssize_t written = -1;
	do
	{
		written = write(fd, report, length);
	} while (written < 0 && errno == EINTR);
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

static void reset_keyboard_state(HidState* hid, bool desynced)
{
	memset(hid->usages, 0, sizeof(hid->usages));
	hid->modifiers = 0;
	hid->keyboard_desynced = desynced;
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

uint16_t hid_clamp_absolute(uint16_t value, uint16_t dimension)
{
	if (dimension == 0)
		return 0;
	return value < dimension ? value : (uint16_t)(dimension - 1U);
}

void hid_map_scancode(uint8_t code, bool extended, bool swap_alt_command,
                      bool right_alt_as_capslock,
                      uint8_t* mapped_code, bool* mapped_extended)
{
	*mapped_code = code;
	*mapped_extended = extended;
	if (right_alt_as_capslock && extended && code == 0x38)
	{
		*mapped_code = 0x3a;
		*mapped_extended = false;
		return;
	}
	if (!swap_alt_command)
		return;
	if (code == 0x38)
	{
		*mapped_code = extended ? 0x5c : 0x5b;
		*mapped_extended = true;
	}
	else if (extended && code == 0x5b)
	{
		*mapped_code = 0x38;
		*mapped_extended = false;
	}
	else if (extended && code == 0x5c)
	{
		*mapped_code = 0x38;
		*mapped_extended = true;
	}
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
	if (hid->keyboard_desynced)
	{
		const uint8_t release[8] = { 0 };
		if (!write_report(hid->keyboard_path, release, sizeof(release)))
		{
			reset_keyboard_state(hid, true);
			return false;
		}
		hid->keyboard_desynced = false;
	}
	if (write_report(hid->keyboard_path, report, sizeof(report)))
		return true;
	reset_keyboard_state(hid, true);
	return false;
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

typedef struct
{
	uint8_t modifier;
	uint8_t usage;
} HidTextKey;

static bool ascii_to_hid(uint8_t character, HidTextKey* key)
{
	if (character >= 'a' && character <= 'z')
	{
		key->modifier = 0;
		key->usage = (uint8_t)(0x04U + character - 'a');
		return true;
	}
	if (character >= 'A' && character <= 'Z')
	{
		key->modifier = 0x02;
		key->usage = (uint8_t)(0x04U + character - 'A');
		return true;
	}
	if (character >= '1' && character <= '9')
	{
		key->modifier = 0;
		key->usage = (uint8_t)(0x1eU + character - '1');
		return true;
	}
	if (character == '0')
	{
		key->modifier = 0;
		key->usage = 0x27;
		return true;
	}

	switch (character)
	{
		case '\b': key->modifier = 0; key->usage = 0x2a; return true;
		case '\t': key->modifier = 0; key->usage = 0x2b; return true;
		case '\n': case '\r': key->modifier = 0; key->usage = 0x28; return true;
		case '\x1b': key->modifier = 0; key->usage = 0x29; return true;
		case ' ': key->modifier = 0; key->usage = 0x2c; return true;
		case '-': key->modifier = 0; key->usage = 0x2d; return true;
		case '_': key->modifier = 0x02; key->usage = 0x2d; return true;
		case '=': key->modifier = 0; key->usage = 0x2e; return true;
		case '+': key->modifier = 0x02; key->usage = 0x2e; return true;
		case '[': key->modifier = 0; key->usage = 0x2f; return true;
		case '{': key->modifier = 0x02; key->usage = 0x2f; return true;
		case ']': key->modifier = 0; key->usage = 0x30; return true;
		case '}': key->modifier = 0x02; key->usage = 0x30; return true;
		case '\\': key->modifier = 0; key->usage = 0x31; return true;
		case '|': key->modifier = 0x02; key->usage = 0x31; return true;
		case ';': key->modifier = 0; key->usage = 0x33; return true;
		case ':': key->modifier = 0x02; key->usage = 0x33; return true;
		case '\'': key->modifier = 0; key->usage = 0x34; return true;
		case '"': key->modifier = 0x02; key->usage = 0x34; return true;
		case '`': key->modifier = 0; key->usage = 0x35; return true;
		case '~': key->modifier = 0x02; key->usage = 0x35; return true;
		case ',': key->modifier = 0; key->usage = 0x36; return true;
		case '<': key->modifier = 0x02; key->usage = 0x36; return true;
		case '.': key->modifier = 0; key->usage = 0x37; return true;
		case '>': key->modifier = 0x02; key->usage = 0x37; return true;
		case '/': key->modifier = 0; key->usage = 0x38; return true;
		case '?': key->modifier = 0x02; key->usage = 0x38; return true;
		case '!': key->modifier = 0x02; key->usage = 0x1e; return true;
		case '@': key->modifier = 0x02; key->usage = 0x1f; return true;
		case '#': key->modifier = 0x02; key->usage = 0x20; return true;
		case '$': key->modifier = 0x02; key->usage = 0x21; return true;
		case '%': key->modifier = 0x02; key->usage = 0x22; return true;
		case '^': key->modifier = 0x02; key->usage = 0x23; return true;
		case '&': key->modifier = 0x02; key->usage = 0x24; return true;
		case '*': key->modifier = 0x02; key->usage = 0x25; return true;
		case '(': key->modifier = 0x02; key->usage = 0x26; return true;
		case ')': key->modifier = 0x02; key->usage = 0x27; return true;
		default: return false;
	}
}

static const char* hangul_jamo_sequence(uint32_t codepoint)
{
	switch (codepoint)
	{
		case 0x3131: return "r"; case 0x3132: return "R"; case 0x3133: return "rt";
		case 0x3134: return "s"; case 0x3135: return "sw"; case 0x3136: return "sg";
		case 0x3137: return "e"; case 0x3138: return "E"; case 0x3139: return "f";
		case 0x313a: return "fr"; case 0x313b: return "fa"; case 0x313c: return "fq";
		case 0x313d: return "ft"; case 0x313e: return "fx"; case 0x313f: return "fv";
		case 0x3140: return "fg"; case 0x3141: return "a"; case 0x3142: return "q";
		case 0x3143: return "Q"; case 0x3144: return "qt"; case 0x3145: return "t";
		case 0x3146: return "T"; case 0x3147: return "d"; case 0x3148: return "w";
		case 0x3149: return "W"; case 0x314a: return "c"; case 0x314b: return "z";
		case 0x314c: return "x"; case 0x314d: return "v"; case 0x314e: return "g";
		case 0x314f: return "k"; case 0x3150: return "o"; case 0x3151: return "i";
		case 0x3152: return "O"; case 0x3153: return "j"; case 0x3154: return "p";
		case 0x3155: return "u"; case 0x3156: return "P"; case 0x3157: return "h";
		case 0x3158: return "hk"; case 0x3159: return "ho"; case 0x315a: return "hl";
		case 0x315b: return "y"; case 0x315c: return "n"; case 0x315d: return "nj";
		case 0x315e: return "np"; case 0x315f: return "nl"; case 0x3160: return "b";
		case 0x3161: return "m"; case 0x3162: return "ml"; case 0x3163: return "l";
		default: return NULL;
	}
}

static bool codepoint_to_hangul_sequences(uint32_t codepoint, const char** lead, const char** vowel,
                                           const char** trail)
{
	static const char* const lead_keys[] = {
		"r", "R", "s", "e", "E", "f", "a", "q", "Q", "t", "T", "d", "w", "W", "c", "z", "x", "v", "g",
	};
	static const char* const vowel_keys[] = {
		"k", "o", "i", "O", "j", "p", "u", "P", "h", "hk", "ho", "hl", "y", "n", "nj", "np", "nl", "b", "m", "ml", "l",
	};
	static const char* const trail_keys[] = {
		"", "r", "R", "rt", "s", "sw", "sg", "e", "f", "fr", "fa", "fq", "ft", "fx", "fv", "fg", "a", "q", "qt", "t", "T", "d", "w", "c", "z", "x", "v", "g",
	};

	if (codepoint < 0xac00 || codepoint > 0xd7a3)
		return false;
	const uint32_t offset = codepoint - 0xac00;
	const uint32_t lead_index = offset / (21U * 28U);
	const uint32_t vowel_index = (offset % (21U * 28U)) / 28U;
	const uint32_t trail_index = offset % 28U;
	*lead = lead_keys[lead_index];
	*vowel = vowel_keys[vowel_index];
	*trail = trail_keys[trail_index];
	return true;
}


static bool is_utf8_continuation(uint8_t byte)
{
	return (byte & 0xc0U) == 0x80U;
}

static bool utf8_decode(const uint8_t* text, size_t length, size_t* offset, uint32_t* codepoint)
{
	if (*offset >= length)
		return false;
	const uint8_t first = text[(*offset)++];
	if (first < 0x80U)
	{
		*codepoint = first;
		return true;
	}
	if (first >= 0xc2U && first <= 0xdfU)
	{
		if (*offset >= length || !is_utf8_continuation(text[*offset]))
			return false;
		*codepoint = ((uint32_t)(first & 0x1fU) << 6U) | (text[(*offset)++] & 0x3fU);
		return true;
	}
	if (first >= 0xe0U && first <= 0xefU)
	{
		if (*offset + 1U >= length || !is_utf8_continuation(text[*offset]) ||
		    !is_utf8_continuation(text[*offset + 1U]) ||
		    (first == 0xe0U && text[*offset] < 0xa0U) ||
		    (first == 0xedU && text[*offset] > 0x9fU))
			return false;
		const uint8_t second = text[(*offset)++];
		const uint8_t third = text[(*offset)++];
		*codepoint = ((uint32_t)(first & 0x0fU) << 12U) |
		             ((uint32_t)(second & 0x3fU) << 6U) |
		             (third & 0x3fU);
		return true;
	}
	if (first >= 0xf0U && first <= 0xf4U)
	{
		if (*offset + 2U >= length || !is_utf8_continuation(text[*offset]) ||
		    !is_utf8_continuation(text[*offset + 1U]) || !is_utf8_continuation(text[*offset + 2U]) ||
		    (first == 0xf0U && text[*offset] < 0x90U) ||
		    (first == 0xf4U && text[*offset] > 0x8fU))
			return false;
		const uint8_t second = text[(*offset)++];
		const uint8_t third = text[(*offset)++];
		const uint8_t fourth = text[(*offset)++];
		*codepoint = ((uint32_t)(first & 0x07U) << 18U) |
		             ((uint32_t)(second & 0x3fU) << 12U) |
		             ((uint32_t)(third & 0x3fU) << 6U) |
		             (fourth & 0x3fU);
		return true;
	}
	return false;
}

static bool text_supported(const uint8_t* text, size_t length)
{
	for (size_t offset = 0; offset < length;)
	{
		uint32_t codepoint = 0;
		HidTextKey key = { 0 };
		const char* lead = NULL;
		const char* vowel = NULL;
		const char* trail = NULL;
		if (!utf8_decode(text, length, &offset, &codepoint) ||
		    !((codepoint != 0 && codepoint <= 0x7fU && ascii_to_hid((uint8_t)codepoint, &key)) ||
		      codepoint_to_hangul_sequences(codepoint, &lead, &vowel, &trail) ||
		      hangul_jamo_sequence(codepoint) != NULL))
			return false;
	}
	return true;
}

static void sleep_milliseconds(long milliseconds)
{
	const struct timespec duration = { .tv_sec = milliseconds / 1000L,
	                                  .tv_nsec = (milliseconds % 1000L) * 1000000L };
	(void)nanosleep(&duration, NULL);
}

static bool tap_text_key(HidState* hid, HidTextKey key)
{
	hid->modifiers = key.modifier;
	hid->usages[key.usage] = true;
	if (!send_keyboard(hid))
		return false;
	sleep_milliseconds(8);
	hid->modifiers = 0;
	hid->usages[key.usage] = false;
	if (!send_keyboard(hid))
		return false;
	sleep_milliseconds(2);
	return true;
}

static bool type_ascii_sequence(HidState* hid, const char* sequence)
{
	for (; *sequence != '\0'; sequence++)
	{
		HidTextKey key = { 0 };
		if (!ascii_to_hid((uint8_t)*sequence, &key) || !tap_text_key(hid, key))
			return false;
	}
	return true;
}

static bool type_codepoint(HidState* hid, uint32_t codepoint)
{
	HidTextKey key = { 0 };
	if (codepoint <= 0x7fU && ascii_to_hid((uint8_t)codepoint, &key))
		return tap_text_key(hid, key);

	const char* lead = NULL;
	const char* vowel = NULL;
	const char* trail = NULL;
	if (codepoint_to_hangul_sequences(codepoint, &lead, &vowel, &trail))
		return type_ascii_sequence(hid, lead) && type_ascii_sequence(hid, vowel) &&
		       type_ascii_sequence(hid, trail);

	const char* sequence = hangul_jamo_sequence(codepoint);
	return sequence && type_ascii_sequence(hid, sequence);
}

bool hid_type_utf8(HidState* hid, const uint8_t* text, size_t length)
{
	if (!hid || !text || length == 0 || !text_supported(text, length))
		return false;

	reset_keyboard_state(hid, false);
	if (!send_keyboard(hid))
		return false;

	for (size_t offset = 0; offset < length;)
	{
		uint32_t codepoint = 0;
		if (!utf8_decode(text, length, &offset, &codepoint) || !type_codepoint(hid, codepoint))
		{
			reset_keyboard_state(hid, false);
			(void)send_keyboard(hid);
			return false;
		}
	}
	return true;
}

static uint8_t touch_buttons(uint8_t buttons)
{
	return buttons & HID_TOUCH_BUTTONS_MASK;
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
	if ((flags & PTR_XFLAGS_BUTTON1) != 0)
	{
		if ((flags & PTR_FLAGS_DOWN) != 0)
			hid->buttons |= HID_TOUCH_XBUTTON1;
		else
			hid->buttons &= (uint8_t)~HID_TOUCH_XBUTTON1;
	}
	if ((flags & PTR_XFLAGS_BUTTON2) != 0)
	{
		if ((flags & PTR_FLAGS_DOWN) != 0)
			hid->buttons |= HID_TOUCH_XBUTTON2;
		else
			hid->buttons &= (uint8_t)~HID_TOUCH_XBUTTON2;
	}

	hid->last_x = hid_scale_absolute(x, width);
	hid->last_y = hid_scale_absolute(y, height);
	uint8_t report[6] = { touch_buttons(hid->buttons), (uint8_t)(hid->last_x & 0xffU),
		(uint8_t)(hid->last_x >> 8U), (uint8_t)(hid->last_y & 0xffU),
		(uint8_t)(hid->last_y >> 8U), 0 };
	return write_report(hid->touch_path, report, sizeof(report));
}

bool hid_relative(HidState* hid, int16_t x, int16_t y, uint8_t buttons)
{
	if (x > 127)
		x = 127;
	if (x < -127)
		x = -127;
	if (y > 127)
		y = 127;
	if (y < -127)
		y = -127;
	hid->mouse_buttons = buttons & HID_MOUSE_BUTTONS_MASK;
	const uint8_t report[4] = { hid->mouse_buttons, (uint8_t)(int8_t)x, (uint8_t)(int8_t)y, 0 };
	return write_report(hid->mouse_path, report, sizeof(report));
}

bool hid_wheel(HidState* hid, uint16_t flags)
{
	if ((flags & PTR_FLAGS_WHEEL) == 0)
		return true;
	int delta = (int)(flags & WHEEL_ROTATION_MASK);
	if ((flags & PTR_FLAGS_WHEEL_NEGATIVE) != 0)
		delta = -delta;
	int detents = delta / 120;
	if (detents == 0 && delta != 0)
		detents = delta > 0 ? 1 : -1;
	if (detents > 1)
		detents = 1;
	if (detents < -1)
		detents = -1;
	/* HID wheel은 한 이벤트당 ±1 노치만 전달. 트랙패드의 큰 delta는 클램프하고,
	 * 트랙패드의 연속 이벤트로 자연스럽게 여러 노치를 스크롤한다.
	 * wheel report는 버튼/이동과 동일한 hidg2(절대 마우스, 6바이트)로 보내
	 * hidg1/hidg2 두 마우스 디바이스의 버튼 상태 충돌을 피한다. */
	uint8_t report[6] = { touch_buttons(hid->buttons), (uint8_t)(hid->last_x & 0xffU),
		(uint8_t)(hid->last_x >> 8U), (uint8_t)(hid->last_y & 0xffU),
		(uint8_t)(hid->last_y >> 8U), (uint8_t)(int8_t)detents };
	return write_report(hid->touch_path, report, sizeof(report));
}

void hid_release_all(HidState* hid)
{
	reset_keyboard_state(hid, false);
	hid->buttons = 0;
	hid->mouse_buttons = 0;
	const uint8_t keyboard[8] = { 0 };
	const uint8_t mouse[4] = { 0 };
	const uint8_t touch[6] = { 0, (uint8_t)(hid->last_x & 0xffU),
		(uint8_t)(hid->last_x >> 8U), (uint8_t)(hid->last_y & 0xffU),
		(uint8_t)(hid->last_y >> 8U), 0 };
	if (!write_report(hid->keyboard_path, keyboard, sizeof(keyboard)))
		hid->keyboard_desynced = true;
	(void)write_report(hid->mouse_path, mouse, sizeof(mouse));
	(void)write_report(hid->touch_path, touch, sizeof(touch));
}
