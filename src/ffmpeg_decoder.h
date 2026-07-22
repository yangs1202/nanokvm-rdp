#ifndef NANOKVM_RDP_FFMPEG_DECODER_H
#define NANOKVM_RDP_FFMPEG_DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef bool (*FfmpegFrameHandler)(void* context, const uint8_t* bgra, size_t length);

typedef struct
{
	pid_t pid;
	int input;
	int output;
	uint8_t* frame;
	size_t frame_size;
	size_t frame_used;
	FfmpegFrameHandler frame_handler;
	void* frame_context;
} FfmpegDecoder;

bool ffmpeg_decoder_start(FfmpegDecoder* decoder, uint16_t width, uint16_t height,
	                      FfmpegFrameHandler frame_handler, void* frame_context);
bool ffmpeg_decoder_push(FfmpegDecoder* decoder, const uint8_t* data, size_t length);
void ffmpeg_decoder_stop(FfmpegDecoder* decoder);

#endif
