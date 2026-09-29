#include "input_router.h"
#include "protocol.h"

#include <assert.h>
#include <string.h>

_Static_assert(NANOKVM_CONTROL_KEY == 5, "v1 key wire type changed");
_Static_assert(NANOKVM_CONTROL_POINTER_ABS == 6, "v1 absolute-pointer wire type changed");
_Static_assert(NANOKVM_CONTROL_POINTER_REL == 7, "v1 relative-pointer wire type changed");
_Static_assert(NANOKVM_CONTROL_WHEEL == 8, "v1 wheel wire type changed");
_Static_assert(NANOKVM_CONTROL_RELEASE_ALL == 9, "v1 release wire type changed");
_Static_assert(NANOKVM_CONTROL_TEXT_UTF8 == 14, "v1 text wire type changed");
_Static_assert(NANOKVM_CONTROL_SYNCHRONIZE == 16, "v1 synchronize wire type changed");

static void expect_message(const InputRouterMessage* message, uint8_t type,
	                       const uint8_t* payload, uint16_t length)
{
	assert(message->type == type);
	assert(message->length == length);
	assert(memcmp(message->payload, payload, length) == 0);
}

static void test_modifier_and_scancode_mapping(void)
{
	InputRouter router;
	InputRouterMessage message;
	input_router_init(&router, 1920, 1080, true);

	input_router_keyboard(&router, 0x38, false, false, &message);
	expect_message(&message, NANOKVM_CONTROL_KEY, (const uint8_t[]){ 0x5b, 1, 0 }, 3);
	assert(router.keyboard_modifiers == 0x08);
	input_router_keyboard(&router, 0x38, false, true, &message);
	expect_message(&message, NANOKVM_CONTROL_KEY, (const uint8_t[]){ 0x5b, 1, 1 }, 3);
	assert(router.keyboard_modifiers == 0);

	input_router_keyboard(&router, 0x1d, false, false, &message);
	expect_message(&message, NANOKVM_CONTROL_KEY, (const uint8_t[]){ 0x1d, 0, 0 }, 3);
	assert(router.keyboard_modifiers == 0x01);
	input_router_keyboard(&router, 0x1d, false, true, &message);
	assert(router.keyboard_modifiers == 0);
}

static void test_unicode_utf8_and_utf16_limits(void)
{
	InputRouter router;
	InputRouterMessage message;
	input_router_init(&router, 1920, 1080, false);

	assert(input_router_unicode(&router, '$', false, &message));
	expect_message(&message, NANOKVM_CONTROL_TEXT_UTF8, (const uint8_t[]){ '$' }, 1);
	assert(input_router_unicode(&router, 0x07ff, false, &message));
	expect_message(&message, NANOKVM_CONTROL_TEXT_UTF8, (const uint8_t[]){ 0xdf, 0xbf }, 2);
	assert(input_router_unicode(&router, 0xac00, false, &message));
	expect_message(&message, NANOKVM_CONTROL_TEXT_UTF8, (const uint8_t[]){ 0xea, 0xb0, 0x80 }, 3);
	assert(input_router_unicode(&router, 0xac00, true, &message));
	assert(message.type == 0 && message.length == 0);
	assert(!input_router_unicode(&router, 0, false, &message));
	assert(!input_router_unicode(&router, 0xd800, false, &message));
	assert(!input_router_unicode(&router, 0xdc00, false, &message));
}

static void test_synchronize_and_release_reset_state(void)
{
	InputRouter router;
	InputRouterMessage message;
	input_router_init(&router, 1920, 1080, false);
	input_router_keyboard(&router, 0x1d, false, false, &message);
	input_router_absolute_pointer(&router, 10, 10, 0x9000, false, &message);
	assert(router.keyboard_modifiers == 0x01 && router.pointer_buttons == 0x01);
	input_router_synchronize(&router, &message);
	expect_message(&message, NANOKVM_CONTROL_SYNCHRONIZE, NULL, 0);
	assert(router.keyboard_modifiers == 0 && router.pointer_buttons == 0 && !router.control_space_down);
	input_router_keyboard(&router, 0x1d, false, false, &message);
	input_router_relative_pointer(&router, 1, -1, 0x9000, false, &message);
	input_router_release_all(&router, &message);
	expect_message(&message, NANOKVM_CONTROL_RELEASE_ALL, NULL, 0);
	assert(router.keyboard_modifiers == 0 && router.pointer_buttons == 0 && !router.control_space_down);
}

static void test_pointer_and_wheel_payloads(void)
{
	InputRouter router;
	InputRouterMessage message;
	input_router_init(&router, 1920, 1080, false);
	input_router_absolute_pointer(&router, 3000, 2000, 0x9000, false, &message);
	expect_message(&message, NANOKVM_CONTROL_POINTER_ABS,
	               (const uint8_t[]){ 0x07, 0x7f, 0x04, 0x37, 0x07, 0x80, 0x04, 0x38, 0x90, 0x00, 0, 0 }, 12);
	assert(router.pointer_buttons == 0x01);
	input_router_absolute_pointer(&router, 0, 0, 0x02ff, true, &message);
	expect_message(&message, NANOKVM_CONTROL_WHEEL, (const uint8_t[]){ 0x02, 0xff }, 2);
	assert(router.pointer_buttons == 0x01);
	input_router_relative_pointer(&router, -2, 3, 0x8000, false, &message);
	expect_message(&message, NANOKVM_CONTROL_POINTER_REL,
	               (const uint8_t[]){ 0xff, 0xfe, 0x00, 0x03, 0x01 }, 5);
	input_router_relative_pointer(&router, 12, 0, 0x0501, true, &message);
	expect_message(&message, NANOKVM_CONTROL_WHEEL, (const uint8_t[]){ 0x05, 0x01 }, 2);
}

int main(void)
{
	test_modifier_and_scancode_mapping();
	test_unicode_utf8_and_utf16_limits();
	test_synchronize_and_release_reset_state();
	test_pointer_and_wheel_payloads();
	return 0;
}
