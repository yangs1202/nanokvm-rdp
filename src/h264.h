#ifndef NANOKVM_RDP_H264_H
#define NANOKVM_RDP_H264_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
	uint8_t* data;
	size_t length;
	size_t capacity;
} H264Buffer;

typedef struct
{
	H264Buffer sps;
	H264Buffer pps;
	H264Buffer scratch;
} H264ParameterSets;

uint32_t h264_nal_type_mask(const uint8_t* data, size_t length);
bool h264_contains_nal_type(const uint8_t* data, size_t length, uint8_t type);
bool h264_has_annexb_start_code(const uint8_t* data, size_t length);
size_t h264_annexb_size(const uint8_t* data, size_t length);
size_t h264_copy_annexb(uint8_t* destination, const uint8_t* data, size_t length);
void h264_parameter_sets_update(H264ParameterSets* parameter_sets, const uint8_t* au,
                                size_t length, uint32_t nal_mask);
/* Prepends cached SPS/PPS that are absent from the AU. Missing cached sets are
 * skipped; if nothing can be prepended, out points to au unchanged. */
bool h264_parameter_sets_prepare_idr(H264ParameterSets* parameter_sets, const uint8_t* au,
                                     size_t length, uint32_t nal_mask, const uint8_t** out,
                                     size_t* out_length);
void h264_parameter_sets_free(H264ParameterSets* parameter_sets);
/* Returns data itself when it already starts with an Annex-B start code (the
 * normal RTP case); otherwise copies it behind a 4-byte start code into the
 * reusable buffer. False only on allocation failure. */
bool h264_ensure_annexb(H264Buffer* buffer, const uint8_t* data, size_t length,
                        const uint8_t** out, size_t* out_length);
void h264_buffer_free(H264Buffer* buffer);
/* Copies one Annex-B NAL of the requested type, including its start code.
 * Parameter sets often share an access unit with an IDR; callers that prepend
 * a cached SPS/PPS must not copy that whole unit or the IDR is duplicated. */
bool h264_copy_nal(uint8_t** destination, size_t* destination_length, const uint8_t* data,
                   size_t length, uint8_t type);

#endif
