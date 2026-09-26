#include "bitmap_diff.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	/* A dark page repaint formerly left 64-pixel rectangles at the old color. */
	uint8_t before[68 * 65 * 4] = { 0 };
	uint8_t after[sizeof(before)];
	memset(before, 16, sizeof(before));
	memcpy(after, before, sizeof(after));
	assert(bitmap_tile_changed(NULL, after, 68, 0, 0, 64, 64));
	assert(!bitmap_tile_changed(before, after, 68, 0, 0, 64, 64));
	memset(after, 23, sizeof(after));
	assert(bitmap_tile_changed(before, after, 68, 0, 0, 64, 64));
	/* Even one low-contrast pixel must not be forgotten by the sent-frame cache. */
	memcpy(after, before, sizeof(after));
	after[((size_t)64 * 68 + 67) * 4 + 1]++;
	assert(bitmap_tile_changed(before, after, 68, 64, 64, 4, 1));
	assert(!bitmap_tile_changed(before, after, 68, 0, 0, 64, 64));
	/* BGRX padding is not visible and must not trigger endless updates. */
	memcpy(after, before, sizeof(after));
	for (size_t i = 3; i < sizeof(after); i += 4) after[i] = 255;
	assert(!bitmap_tile_changed(before, after, 68, 0, 0, 64, 64));
	puts("Bitmap diff: dark repaint, single pixel, partial edge and ignored padding passed");
	return 0;
}
