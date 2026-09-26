#include "h264.h"

#include <string.h>

bool h264_has_annexb_start_code(const uint8_t* data, size_t length)
{
	return length >= 3 && data[0] == 0 && data[1] == 0 &&
	       (data[2] == 1 || (length >= 4 && data[2] == 0 && data[3] == 1));
}

bool h264_contains_nal_type(const uint8_t* data, size_t length, uint8_t type)
{
	for (size_t index = 0; index + 3 < length; index++)
	{
		size_t start_length = 0;
		if (data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 1)
			start_length = 3;
		else if (index + 4 < length && data[index] == 0 && data[index + 1] == 0 &&
		         data[index + 2] == 0 && data[index + 3] == 1)
			start_length = 4;
		if (start_length != 0 && index + start_length < length &&
		    (data[index + start_length] & 0x1fU) == type)
			return true;
	}
	return false;
}

size_t h264_annexb_size(const uint8_t* data, size_t length)
{
	return length == 0 ? 0 : length + (h264_has_annexb_start_code(data, length) ? 0 : 4);
}

size_t h264_copy_annexb(uint8_t* destination, const uint8_t* data, size_t length)
{
	if (length == 0)
		return 0;
	if (h264_has_annexb_start_code(data, length))
	{
		memcpy(destination, data, length);
		return length;
	}
	destination[0] = 0;
	destination[1] = 0;
	destination[2] = 0;
	destination[3] = 1;
	memcpy(destination + 4, data, length);
	return length + 4;
}
