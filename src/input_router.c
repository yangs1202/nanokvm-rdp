#include "input_router.h"

#include "hid.h"
#include "protocol.h"

#include <string.h>

static void clear_message(InputRouterMessage* message)
{
	memset(message, 0, sizeof(*message));
}

void input_router_init(InputRouter* router, uint16_t width, uint16_t height,
	                   bool swap_alt_command)
{
	memset(router, 0, sizeof(*router));
	router->width = width;
	router->height = height;
	router->swap_alt_command = swap_alt_command;
}

void input_router_keyboard(InputRouter* router, uint8_t code, bool extended,
	                       bool release, InputRouterMessage* message)
{
	uint8_t mapped_code = code;
	bool mapped_extended = extended;
	uint8_t usage = 0;
	uint8_t modifier = 0;

	hid_map_scancode(code, extended, router->swap_alt_command, &mapped_code, &mapped_extended);
	(void)hid_translate_scancode(mapped_code, mapped_extended, &usage, &modifier);
	if (release)
		router->keyboard_modifiers &= (uint8_t)~modifier;
	else
		router->keyboard_modifiers |= modifier;
	if (code == 0x39 && !extended &&
	    ((router->keyboard_modifiers & 0x11U) || router->control_space_down))
		router->control_space_down = !release;

	clear_message(message);
	message->type = NANOKVM_CONTROL_KEY;
	message->payload[0] = mapped_code;
	message->payload[1] = mapped_extended;
	message->payload[2] = release;
	message->length = NANOKVM_KEY_PAYLOAD_SIZE;
}

bool input_router_unicode(InputRouter* router, uint16_t code, bool release,
	                      InputRouterMessage* message)
{
	(void)router;
	clear_message(message);
	if (release)
		return true;
	if (code == 0 || (code >= 0xd800U && code <= 0xdfffU))
		return false;

	message->type = NANOKVM_CONTROL_TEXT_UTF8;
	if (code <= 0x007fU)
	{
		message->payload[0] = (uint8_t)code;
		message->length = 1;
	}
	else if (code <= 0x07ffU)
	{
		message->payload[0] = (uint8_t)(0xc0U | (code >> 6U));
		message->payload[1] = (uint8_t)(0x80U | (code & 0x3fU));
		message->length = 2;
	}
	else
	{
		message->payload[0] = (uint8_t)(0xe0U | (code >> 12U));
		message->payload[1] = (uint8_t)(0x80U | ((code >> 6U) & 0x3fU));
		message->payload[2] = (uint8_t)(0x80U | (code & 0x3fU));
		message->length = 3;
	}
	return true;
}

void input_router_synchronize(InputRouter* router, InputRouterMessage* message)
{
	router->pointer_buttons = 0;
	router->keyboard_modifiers = 0;
	router->control_space_down = false;
	clear_message(message);
	message->type = NANOKVM_CONTROL_SYNCHRONIZE;
}

void input_router_release_all(InputRouter* router, InputRouterMessage* message)
{
	router->pointer_buttons = 0;
	router->keyboard_modifiers = 0;
	router->control_space_down = false;
	clear_message(message);
	message->type = NANOKVM_CONTROL_RELEASE_ALL;
}

void input_router_absolute_pointer(InputRouter* router, uint16_t x, uint16_t y,
	                               uint16_t flags, bool wheel, InputRouterMessage* message)
{
	if (!wheel)
		router->pointer_buttons = hid_pointer_buttons(router->pointer_buttons, flags);
	clear_message(message);
	if (wheel)
	{
		message->type = NANOKVM_CONTROL_WHEEL;
		protocol_write_u16(message->payload, flags);
		message->length = 2;
		return;
	}
	message->type = NANOKVM_CONTROL_POINTER_ABS;
	protocol_write_u16(message->payload, hid_clamp_absolute(x, router->width));
	protocol_write_u16(message->payload + 2, hid_clamp_absolute(y, router->height));
	protocol_write_u16(message->payload + 4, router->width);
	protocol_write_u16(message->payload + 6, router->height);
	protocol_write_u16(message->payload + 8, flags);
	message->length = 12;
}

void input_router_relative_pointer(InputRouter* router, int16_t x_delta, int16_t y_delta,
	                               uint16_t flags, bool wheel, InputRouterMessage* message)
{
	if (!wheel)
		router->pointer_buttons = hid_pointer_buttons(router->pointer_buttons, flags);
	clear_message(message);
	if (wheel)
	{
		message->type = NANOKVM_CONTROL_WHEEL;
		protocol_write_u16(message->payload, flags);
		message->length = 2;
		return;
	}
	message->type = NANOKVM_CONTROL_POINTER_REL;
	protocol_write_u16(message->payload, (uint16_t)x_delta);
	protocol_write_u16(message->payload + 2, (uint16_t)y_delta);
	message->payload[4] = router->pointer_buttons;
	message->length = 5;
}
