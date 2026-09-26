#ifndef NANOKVM_RDP_FFMPEG_DECODER_H
#define NANOKVM_RDP_FFMPEG_DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

typedef bool (*FfmpegFrameHandler)(void* context, const uint8_t* bgra, size_t length);

typedef bool (*FfmpegDecodedHandler)(void* context, const AVFrame* frame);

/* Owned by the rendering thread, separate from the decoder thread. */
typedef struct
{
	struct SwsContext* scaler;
	uint8_t* frame;
	size_t frame_size;
	int scaled_width;
	int scaled_height;
	uint16_t width;
	uint16_t height;
} FfmpegConverter;

bool ffmpeg_converter_convert(FfmpegConverter* converter, const AVFrame* source,
                              uint16_t width, uint16_t height);
void ffmpeg_converter_free(FfmpegConverter* converter);

typedef struct
{
	AVCodecContext* codec;
	AVFrame* decoded;
	AVPacket* packet;
	FfmpegConverter converter;
	uint16_t width;
	uint16_t height;
	FfmpegFrameHandler frame_handler;
	FfmpegDecodedHandler decoded_handler;
	void* frame_context;
} FfmpegDecoder;

bool ffmpeg_decoder_start(FfmpegDecoder* decoder, uint16_t width, uint16_t height,
                          FfmpegFrameHandler frame_handler, void* frame_context);
/* Handler may retain the frame with av_frame_clone; no BGRA work is done. */
bool ffmpeg_decoder_start_raw(FfmpegDecoder* decoder, FfmpegDecodedHandler handler,
                              void* context);
/* Each push must contain one complete Annex-B access unit, including parameter
 * sets when needed. RTP marker boundaries supply this framing. */
bool ffmpeg_decoder_push(FfmpegDecoder* decoder, const uint8_t* data, size_t length);
void ffmpeg_decoder_stop(FfmpegDecoder* decoder);

#endif
