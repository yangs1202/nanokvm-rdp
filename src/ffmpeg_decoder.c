#include "ffmpeg_decoder.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool decoder_receive(FfmpegDecoder* decoder)
{
	for (;;)
	{
		const int result = avcodec_receive_frame(decoder->codec, decoder->decoded);
		if (result == AVERROR(EAGAIN) || result == AVERROR_EOF ||
		    result == AVERROR_INVALIDDATA)
			return true;
		if (result < 0)
			return false;

		const AVFrame* source = decoder->decoded;
		if (source->width <= 0 || source->height <= 0)
			return false;
		int width = decoder->width;
		int height = decoder->height;
		if ((int64_t)source->width * height > (int64_t)source->height * width)
			height = (int)((int64_t)source->height * width / source->width);
		else
			width = (int)((int64_t)source->width * height / source->height);
		if (width < 1) width = 1;
		if (height < 1) height = 1;
		decoder->scaler = sws_getCachedContext(
		    decoder->scaler, source->width, source->height, source->format,
		    width, height, AV_PIX_FMT_BGRA, SWS_BICUBIC, NULL, NULL, NULL);
		if (!decoder->scaler)
			return false;
		const int colorspace = source->colorspace == AVCOL_SPC_UNSPECIFIED
		                           ? SWS_CS_DEFAULT : source->colorspace;
		const int* coefficients = sws_getCoefficients(colorspace);
		if (sws_setColorspaceDetails(decoder->scaler, coefficients,
		                            source->color_range == AVCOL_RANGE_JPEG,
		                            coefficients, 1, 0, 1 << 16, 1 << 16) < 0)
			return false;
		memset(decoder->frame, 0, decoder->frame_size);
		for (size_t i = 3; i < decoder->frame_size; i += 4)
			decoder->frame[i] = 255;
		const int stride = decoder->width * 4;
		const size_t offset = (size_t)((decoder->height - height) / 2) * stride +
		                      (size_t)((decoder->width - width) / 2) * 4;
		uint8_t* output[4] = { decoder->frame + offset, NULL, NULL, NULL };
		const int strides[4] = { stride, 0, 0, 0 };
		if (sws_scale(decoder->scaler, (const uint8_t* const*)source->data,
		              source->linesize, 0, source->height, output, strides) != height)
			return false;
		const bool handled = decoder->frame_handler(decoder->frame_context,
		                                           decoder->frame, decoder->frame_size);
		av_frame_unref(decoder->decoded);
		if (!handled)
			return false;
	}
}

bool ffmpeg_decoder_start(FfmpegDecoder* decoder, uint16_t width, uint16_t height,
                          FfmpegFrameHandler frame_handler, void* frame_context)
{
	if (!decoder || !width || !height || !frame_handler)
		return false;
	*decoder = (FfmpegDecoder){ 0 };
	const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec)
		return false;
	decoder->codec = avcodec_alloc_context3(codec);
	decoder->decoded = av_frame_alloc();
	decoder->packet = av_packet_alloc();
	decoder->frame_size = (size_t)width * height * 4U;
	decoder->frame = malloc(decoder->frame_size);
	if (!decoder->codec || !decoder->decoded || !decoder->packet || !decoder->frame)
		goto fail;
	decoder->codec->thread_count = 1;
	decoder->codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
	if (avcodec_open2(decoder->codec, codec, NULL) < 0)
		goto fail;
	decoder->width = width;
	decoder->height = height;
	decoder->frame_handler = frame_handler;
	decoder->frame_context = frame_context;
	return true;
fail:
	ffmpeg_decoder_stop(decoder);
	return false;
}

bool ffmpeg_decoder_push(FfmpegDecoder* decoder, const uint8_t* data, size_t length)
{
	if (!decoder || !decoder->codec || !data || !length || length > INT_MAX)
		return false;
	/* av_new_packet adds the zero padding required by FFmpeg's bitstream reader. */
	if (av_new_packet(decoder->packet, (int)length) < 0)
		return false;
	memcpy(decoder->packet->data, data, length);
	const int result = avcodec_send_packet(decoder->codec, decoder->packet);
	av_packet_unref(decoder->packet);
	/* Loss recovery requests an IDR on the control channel. A damaged access
	 * unit must not tear down the RDP session while that IDR is in flight. */
	return result == AVERROR_INVALIDDATA || (result >= 0 && decoder_receive(decoder));
}

void ffmpeg_decoder_stop(FfmpegDecoder* decoder)
{
	if (!decoder)
		return;
	sws_freeContext(decoder->scaler);
	av_packet_free(&decoder->packet);
	av_frame_free(&decoder->decoded);
	avcodec_free_context(&decoder->codec);
	free(decoder->frame);
	*decoder = (FfmpegDecoder){ 0 };
}
