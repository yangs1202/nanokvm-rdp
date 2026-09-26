#ifndef NANOKVM_BITMAP_DIFF_H
#define NANOKVM_BITMAP_DIFF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool bitmap_tile_changed(const uint8_t* previous, const uint8_t* current,
                                       uint16_t width, uint16_t left, uint16_t top,
                                       uint16_t columns, uint16_t rows)
{
	if (!previous)
		return true;
	/* A threshold can leave dark background tiles or small cursor/text changes
	 * stale forever. Compare every visible channel; the fourth byte is padding. */
	for (uint16_t row = 0; row < rows; row++)
	{
		const size_t offset = ((size_t)(top + row) * width + left) * 4U;
		for (uint16_t column = 0; column < columns; column++)
		{
			const size_t pixel = offset + (size_t)column * 4U;
			if (previous[pixel] != current[pixel] ||
			    previous[pixel + 1] != current[pixel + 1] ||
			    previous[pixel + 2] != current[pixel + 2])
				return true;
		}
	}
	return false;
}

#endif
