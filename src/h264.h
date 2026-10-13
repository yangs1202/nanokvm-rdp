#ifndef NANOKVM_RDP_H264_H
#define NANOKVM_RDP_H264_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool h264_contains_nal_type(const uint8_t* data, size_t length, uint8_t type);
bool h264_has_annexb_start_code(const uint8_t* data, size_t length);
size_t h264_annexb_size(const uint8_t* data, size_t length);
size_t h264_copy_annexb(uint8_t* destination, const uint8_t* data, size_t length);
/* Copies one Annex-B NAL of the requested type, including its start code.
 * Parameter sets often share an access unit with an IDR; callers that prepend
 * a cached SPS/PPS must not copy that whole unit or the IDR is duplicated. */
bool h264_copy_nal(uint8_t** destination, size_t* destination_length, const uint8_t* data,
                   size_t length, uint8_t type);

#endif
