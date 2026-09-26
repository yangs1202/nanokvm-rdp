#include "ffmpeg_decoder.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

bool ffmpeg_converter_convert(FfmpegConverter* converter, const AVFrame* source,
                              uint16_t output_width, uint16_t output_height)
{
	if (!converter || !source || source->width <= 0 || source->height <= 0 ||
	    !output_width || !output_height)
		return false;
	if (!converter->frame || converter->width != output_width || converter->height != output_height)
	{
		const size_t size = (size_t)output_width * output_height * 4U;
		uint8_t* frame = realloc(converter->frame, size);
		if (!frame) return false;
		converter->frame = frame;
		converter->frame_size = size;
		converter->width = output_width;
		converter->height = output_height;
		converter->scaled_width = 0;
		converter->scaled_height = 0;
	}
	int width = output_width;
	int height = output_height;
	if ((int64_t)source->width * height > (int64_t)source->height * width)
		height = (int)((int64_t)source->height * width / source->width);
	else
		width = (int)((int64_t)source->width * height / source->height);
	if (width < 1) width = 1;
	if (height < 1) height = 1;
	converter->scaler = sws_getCachedContext(
	    converter->scaler, source->width, source->height, source->format,
	    width, height, AV_PIX_FMT_BGRA, SWS_BICUBIC, NULL, NULL, NULL);
	if (!converter->scaler)
		return false;
	const int colorspace = source->colorspace == AVCOL_SPC_UNSPECIFIED
	                           ? SWS_CS_DEFAULT : source->colorspace;
	const int* coefficients = sws_getCoefficients(colorspace);
	if (sws_setColorspaceDetails(converter->scaler, coefficients,
	                            source->color_range == AVCOL_RANGE_JPEG,
	                            coefficients, 1, 0, 1 << 16, 1 << 16) < 0)
		return false;
	/* Initialize only when geometry changes. sws_scale overwrites the image
	 * rectangle; opaque black padding remains valid between frames. */
	if (converter->scaled_width != width || converter->scaled_height != height)
	{
		memset(converter->frame, 0, converter->frame_size);
		for (size_t i = 3; i < converter->frame_size; i += 4)
			converter->frame[i] = 255;
		converter->scaled_width = width;
		converter->scaled_height = height;
	}
	const int stride = converter->width * 4;
	const size_t offset = (size_t)((converter->height - height) / 2) * stride +
	                      (size_t)((converter->width - width) / 2) * 4;
	uint8_t* output[4] = { converter->frame + offset, NULL, NULL, NULL };
	const int strides[4] = { stride, 0, 0, 0 };
	if (sws_scale(converter->scaler, (const uint8_t* const*)source->data,
	              source->linesize, 0, source->height, output, strides) != height)
		return false;
	return true;
}

void ffmpeg_converter_free(FfmpegConverter* converter)
{
	if (!converter) return;
	sws_freeContext(converter->scaler);
	free(converter->frame);
	*converter = (FfmpegConverter){ 0 };
}

static bool decoder_receive(FfmpegDecoder* decoder)
{
	for (;;)
	{
		const int result = avcodec_receive_frame(decoder->codec, decoder->decoded);
		if (result == AVERROR(EAGAIN) || result == AVERROR_EOF ||
		    result == AVERROR_INVALIDDATA)
			return true;
		if (result < 0) return false;
		bool handled;
		if (decoder->decoded_handler)
			handled = decoder->decoded_handler(decoder->frame_context, decoder->decoded);
		else
			handled = ffmpeg_converter_convert(&decoder->converter, decoder->decoded,
			                                   decoder->width, decoder->height) &&
			          decoder->frame_handler(decoder->frame_context, decoder->converter.frame,
			                                 decoder->converter.frame_size);
		av_frame_unref(decoder->decoded);
		if (!handled) return false;
	}
}

static bool decoder_start(FfmpegDecoder* decoder, uint16_t width, uint16_t height,
                          FfmpegFrameHandler frame_handler, FfmpegDecodedHandler decoded_handler,
                          void* frame_context)
{
	if (!decoder || (!decoded_handler && (!width || !height || !frame_handler)))
		return false;
	*decoder = (FfmpegDecoder){ 0 };
	const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec)
		return false;
	decoder->codec = avcodec_alloc_context3(codec);
	decoder->decoded = av_frame_alloc();
	decoder->packet = av_packet_alloc();
	if (!decoder->codec || !decoder->decoded || !decoder->packet)
		goto fail;
	decoder->codec->thread_count = 1;
	decoder->codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
	if (avcodec_open2(decoder->codec, codec, NULL) < 0)
		goto fail;
	decoder->width = width;
	decoder->height = height;
	decoder->frame_handler = frame_handler;
	decoder->decoded_handler = decoded_handler;
	decoder->frame_context = frame_context;
	return true;
fail:
	ffmpeg_decoder_stop(decoder);
	return false;
}

bool ffmpeg_decoder_start(FfmpegDecoder* decoder, uint16_t width, uint16_t height,
                          FfmpegFrameHandler handler, void* context)
{
	return decoder_start(decoder, width, height, handler, NULL, context);
}

bool ffmpeg_decoder_start_raw(FfmpegDecoder* decoder, FfmpegDecodedHandler handler,
                              void* context)
{
	return handler && decoder_start(decoder, 0, 0, NULL, handler, context);
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
	ffmpeg_converter_free(&decoder->converter);
	av_packet_free(&decoder->packet);
	av_frame_free(&decoder->decoded);
	avcodec_free_context(&decoder->codec);
	*decoder = (FfmpegDecoder){ 0 };
}
