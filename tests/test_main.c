#include "h264.h"
#include "hid.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

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
}

int main(void)
{
	test_h264_annexb();
	test_hid_mapping();
	return 0;
}
