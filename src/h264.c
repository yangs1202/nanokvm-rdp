#include "h264.h"

#include <stdlib.h>
#include <string.h>

bool h264_has_annexb_start_code(const uint8_t* data, size_t length)
{
	return length >= 3 && data[0] == 0 && data[1] == 0 &&
	       (data[2] == 1 || (length >= 4 && data[2] == 0 && data[3] == 1));
}

uint32_t h264_nal_type_mask(const uint8_t* data, size_t length)
{
	uint32_t mask = 0;
	if (!data)
		return 0;
	for (size_t index = 0; index + 3 < length; index++)
	{
		size_t start_length = 0;
		if (data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 1)
			start_length = 3;
		else if (index + 4 < length && data[index] == 0 && data[index + 1] == 0 &&
		         data[index + 2] == 0 && data[index + 3] == 1)
			start_length = 4;
		if (start_length != 0 && index + start_length < length)
			mask |= 1U << (data[index + start_length] & 0x1fU);
	}
	return mask;
}

bool h264_contains_nal_type(const uint8_t* data, size_t length, uint8_t type)
{
	return type < 32 && (h264_nal_type_mask(data, length) & (1U << type)) != 0;
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

static bool nal_start(const uint8_t* data, size_t length, size_t index, size_t* start_length)
{
	if (index + 3 >= length || data[index] != 0 || data[index + 1] != 0)
		return false;
	if (data[index + 2] == 1)
	{
		*start_length = 3;
		return true;
	}
	if (index + 4 < length && data[index + 2] == 0 && data[index + 3] == 1)
	{
		*start_length = 4;
		return true;
	}
	return false;
}

static bool find_nal(const uint8_t* data, size_t length, uint8_t type, size_t* start,
                     size_t* nal_length)
{
	for (size_t index = 0; index + 3 < length; index++)
	{
		size_t start_length = 0;
		if (!nal_start(data, length, index, &start_length) ||
		    index + start_length >= length ||
		    (data[index + start_length] & 0x1fU) != type)
			continue;
		size_t end = index + start_length + 1;
		while (end + 2 < length)
		{
			size_t ignored = 0;
			if (nal_start(data, length, end, &ignored))
				break;
			end++;
		}
		if (end + 2 >= length)
			end = length;
		*start = index;
		*nal_length = end - index;
		return true;
	}
	return false;
}

static bool buffer_copy_nal(H264Buffer* buffer, const uint8_t* data, size_t length, uint8_t type)
{
	size_t start = 0;
	size_t nal_length = 0;
	if (!find_nal(data, length, type, &start, &nal_length))
		return false;
	if (nal_length > buffer->capacity)
	{
		uint8_t* next = realloc(buffer->data, nal_length);
		if (!next)
			return false;
		buffer->data = next;
		buffer->capacity = nal_length;
	}
	memcpy(buffer->data, data + start, nal_length);
	buffer->length = nal_length;
	return true;
}

void h264_parameter_sets_update(H264ParameterSets* parameter_sets, const uint8_t* au,
                                size_t length, uint32_t nal_mask)
{
	if (!parameter_sets || !au)
		return;
	if ((nal_mask & (1U << 7)) != 0)
		(void)buffer_copy_nal(&parameter_sets->sps, au, length, 7);
	if ((nal_mask & (1U << 8)) != 0)
		(void)buffer_copy_nal(&parameter_sets->pps, au, length, 8);
}

bool h264_parameter_sets_prepare_idr(H264ParameterSets* parameter_sets, const uint8_t* au,
                                     size_t length, uint32_t nal_mask, const uint8_t** out,
                                     size_t* out_length)
{
	if (!parameter_sets || !out || !out_length || (!au && length != 0))
		return false;
	const bool has_sps = (nal_mask & (1U << 7)) != 0;
	const bool has_pps = (nal_mask & (1U << 8)) != 0;
	const size_t sps_size = has_sps || !parameter_sets->sps.data
	                            ? 0
	                            : h264_annexb_size(parameter_sets->sps.data,
	                                               parameter_sets->sps.length);
	const size_t pps_size = has_pps || !parameter_sets->pps.data
	                            ? 0
	                            : h264_annexb_size(parameter_sets->pps.data,
	                                               parameter_sets->pps.length);
	if (sps_size > SIZE_MAX - pps_size ||
	    sps_size + pps_size > SIZE_MAX - length)
		return false;
	const size_t combined_length = sps_size + pps_size + length;
	if (sps_size == 0 && pps_size == 0)
	{
		*out = au;
		*out_length = length;
		return true;
	}
	if (combined_length > parameter_sets->scratch.capacity)
	{
		uint8_t* next = realloc(parameter_sets->scratch.data, combined_length);
		if (!next && combined_length != 0)
			return false;
		parameter_sets->scratch.data = next;
		parameter_sets->scratch.capacity = combined_length;
	}
	size_t offset = 0;
	if (sps_size != 0)
		offset += h264_copy_annexb(parameter_sets->scratch.data + offset,
		                           parameter_sets->sps.data, parameter_sets->sps.length);
	if (pps_size != 0)
		offset += h264_copy_annexb(parameter_sets->scratch.data + offset,
		                           parameter_sets->pps.data, parameter_sets->pps.length);
	if (length != 0)
		memcpy(parameter_sets->scratch.data + offset, au, length);
	*out = parameter_sets->scratch.data;
	*out_length = offset + length;
	return true;
}

void h264_parameter_sets_free(H264ParameterSets* parameter_sets)
{
	if (!parameter_sets)
		return;
	free(parameter_sets->sps.data);
	free(parameter_sets->pps.data);
	free(parameter_sets->scratch.data);
	*parameter_sets = (H264ParameterSets){ 0 };
}

bool h264_ensure_annexb(H264Buffer* buffer, const uint8_t* data, size_t length,
                        const uint8_t** out, size_t* out_length)
{
	if (!buffer || !out || !out_length || (!data && length != 0))
		return false;
	if (length == 0 || h264_has_annexb_start_code(data, length))
	{
		*out = data;
		*out_length = length;
		return true;
	}
	const size_t needed = h264_annexb_size(data, length);
	if (needed < length)
		return false;
	if (needed > buffer->capacity)
	{
		uint8_t* next = realloc(buffer->data, needed);
		if (!next)
			return false;
		buffer->data = next;
		buffer->capacity = needed;
	}
	buffer->length = h264_copy_annexb(buffer->data, data, length);
	*out = buffer->data;
	*out_length = buffer->length;
	return true;
}

void h264_buffer_free(H264Buffer* buffer)
{
	if (!buffer)
		return;
	free(buffer->data);
	*buffer = (H264Buffer){ 0 };
}

bool h264_copy_nal(uint8_t** destination, size_t* destination_length, const uint8_t* data,
                   size_t length, uint8_t type)
{
	if (!destination || !destination_length || !data)
		return false;
	for (size_t index = 0; index + 3 < length; index++)
	{
		size_t start_length = 0;
		if (!nal_start(data, length, index, &start_length) ||
		    index + start_length >= length ||
		    (data[index + start_length] & 0x1fU) != type)
			continue;
		size_t end = index + start_length + 1;
		while (end + 2 < length)
		{
			size_t ignored = 0;
			if (nal_start(data, length, end, &ignored))
				break;
			end++;
		}
		if (end + 2 >= length)
			end = length;
		uint8_t* next = malloc(end - index);
		if (!next)
			return false;
		memcpy(next, data + index, end - index);
		free(*destination);
		*destination = next;
		*destination_length = end - index;
		return true;
	}
	return false;
}
