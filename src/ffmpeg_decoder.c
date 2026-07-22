#include "ffmpeg_decoder.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static bool set_nonblocking(int fd)
{
	const int flags = fcntl(fd, F_GETFL, 0);
	return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool decoder_drain(FfmpegDecoder* decoder, int timeout_ms)
{
	struct pollfd pollfd = { .fd = decoder->output, .events = POLLIN };
	const int ready = poll(&pollfd, 1, timeout_ms);
	if (ready < 0 && errno != EINTR)
		return false;
	if (ready <= 0 || (pollfd.revents & (POLLERR | POLLNVAL)) != 0)
		return ready >= 0;

	for (;;)
	{
		uint8_t buffer[32768];
		const ssize_t length = read(decoder->output, buffer, sizeof(buffer));
		if (length > 0)
		{
			size_t offset = 0;
			while (offset < (size_t)length)
			{
				const size_t available = decoder->frame_size - decoder->frame_used;
				const size_t copy_length =
				    available < ((size_t)length - offset) ? available : ((size_t)length - offset);
				memcpy(decoder->frame + decoder->frame_used, buffer + offset, copy_length);
				decoder->frame_used += copy_length;
				offset += copy_length;
				if (decoder->frame_used == decoder->frame_size)
				{
					decoder->frame_used = 0;
					if (!decoder->frame_handler(decoder->frame_context, decoder->frame,
					                             decoder->frame_size))
						return false;
				}
			}
			continue;
		}
		if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return true;
		return length == 0;
	}
}

bool ffmpeg_decoder_start(FfmpegDecoder* decoder, uint16_t width, uint16_t height,
	                      FfmpegFrameHandler frame_handler, void* frame_context)
{
	if (!decoder || width == 0 || height == 0 || !frame_handler)
		return false;
	*decoder = (FfmpegDecoder){ .pid = -1, .input = -1, .output = -1 };
	const size_t frame_size = (size_t)width * height * 4U;
	decoder->frame = malloc(frame_size);
	if (!decoder->frame)
		return false;
	decoder->frame_size = frame_size;
	decoder->frame_handler = frame_handler;
	decoder->frame_context = frame_context;

	int input[2] = { -1, -1 };
	int output[2] = { -1, -1 };
	if (pipe(input) != 0 || pipe(output) != 0)
		goto fail;
	decoder->pid = fork();
	if (decoder->pid < 0)
		goto fail;
	if (decoder->pid == 0)
	{
		char scale[32] = { 0 };
		(void)snprintf(scale, sizeof(scale), "scale=%u:%u", width, height);
		(void)dup2(input[0], STDIN_FILENO);
		(void)dup2(output[1], STDOUT_FILENO);
		(void)close(input[0]);
		(void)close(input[1]);
		(void)close(output[0]);
		(void)close(output[1]);
		execl("/usr/bin/ffmpeg", "ffmpeg", "-loglevel", "error", "-threads", "1", "-fflags",
		      "nobuffer", "-flags", "low_delay", "-f", "h264", "-i", "pipe:0", "-an", "-pix_fmt",
		      "bgra", "-vf", scale, "-f", "rawvideo", "pipe:1", (char*)NULL);
		_exit(127);
	}
	(void)close(input[0]);
	(void)close(output[1]);
	input[0] = -1;
	output[1] = -1;
	decoder->input = input[1];
	decoder->output = output[0];
	input[1] = -1;
	output[0] = -1;
	if (!set_nonblocking(decoder->input) || !set_nonblocking(decoder->output))
		goto fail;
	return true;

fail:
	if (input[0] >= 0)
		(void)close(input[0]);
	if (input[1] >= 0)
		(void)close(input[1]);
	if (output[0] >= 0)
		(void)close(output[0]);
	if (output[1] >= 0)
		(void)close(output[1]);
	ffmpeg_decoder_stop(decoder);
	return false;
}

bool ffmpeg_decoder_push(FfmpegDecoder* decoder, const uint8_t* data, size_t length)
{
	if (!decoder || decoder->input < 0 || !data || length == 0)
		return false;
	while (length > 0)
	{
		const ssize_t written = write(decoder->input, data, length);
		if (written > 0)
		{
			data += written;
			length -= (size_t)written;
			if (!decoder_drain(decoder, 0))
				return false;
			continue;
		}
		if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			return false;

		struct pollfd pollfds[2] = {
			{ .fd = decoder->input, .events = POLLOUT },
			{ .fd = decoder->output, .events = POLLIN },
		};
		const int ready = poll(pollfds, 2, 100);
		if (ready < 0 && errno != EINTR)
			return false;
		if (ready > 0 && !decoder_drain(decoder, 0))
			return false;
	}
	return true;
}

void ffmpeg_decoder_stop(FfmpegDecoder* decoder)
{
	if (!decoder)
		return;
	if (decoder->input >= 0)
	{
		(void)close(decoder->input);
		decoder->input = -1;
	}
	if (decoder->output >= 0)
	{
		for (unsigned index = 0; index < 10; index++)
			(void)decoder_drain(decoder, 100);
		(void)close(decoder->output);
		decoder->output = -1;
	}
	if (decoder->pid > 0)
	{
		int status = 0;
		(void)waitpid(decoder->pid, &status, 0);
		decoder->pid = -1;
	}
	free(decoder->frame);
	*decoder = (FfmpegDecoder){ .pid = -1, .input = -1, .output = -1 };
}
