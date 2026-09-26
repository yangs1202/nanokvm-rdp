#ifndef NANOKVM_RDP_FFMPEG_DECODER_H
#define NANOKVM_RDP_FFMPEG_DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

typedef bool (*FfmpegFrameHandler)(void* context, const uint8_t* bgra, size_t length);

typedef struct
{
	AVCodecContext* codec;
	AVFrame* decoded;
	AVPacket* packet;
	struct SwsContext* scaler;
	uint8_t* frame;
	size_t frame_size;
	uint16_t width;
	uint16_t height;
	FfmpegFrameHandler frame_handler;
	void* frame_context;
} FfmpegDecoder;

bool ffmpeg_decoder_start(FfmpegDecoder* decoder, uint16_t width, uint16_t height,
                          FfmpegFrameHandler frame_handler, void* frame_context);
/* Each push must contain one complete Annex-B access unit, including parameter
 * sets when needed. RTP marker boundaries supply this framing. */
bool ffmpeg_decoder_push(FfmpegDecoder* decoder, const uint8_t* data, size_t length);
void ffmpeg_decoder_stop(FfmpegDecoder* decoder);

#endif
