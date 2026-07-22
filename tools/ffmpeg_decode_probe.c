#include "foldvnc_client.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WIDTH 1920U
#define HEIGHT 1080U
#define PROBE_SECONDS 12U

typedef struct
{
	pid_t pid;
	int input;
	int output;
	uint64_t output_bytes;
	uint32_t frames;
} FfmpegDecoder;

static volatile sig_atomic_t stop_requested = 0;

static void on_signal(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static uint64_t monotonic_milliseconds(void)
{
	struct timespec now = { 0 };
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return ((uint64_t)now.tv_sec * 1000U) + ((uint64_t)now.tv_nsec / 1000000U);
}

static bool set_nonblocking(int fd)
{
	const int flags = fcntl(fd, F_GETFL, 0);
	return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool decoder_start(FfmpegDecoder* decoder)
{
	int input[2] = { -1, -1 };
	int output[2] = { -1, -1 };
	if (pipe(input) != 0 || pipe(output) != 0)
		goto fail;

	decoder->pid = fork();
	if (decoder->pid < 0)
		goto fail;
	if (decoder->pid == 0)
	{
		(void)dup2(input[0], STDIN_FILENO);
		(void)dup2(output[1], STDOUT_FILENO);
		(void)close(input[0]);
		(void)close(input[1]);
		(void)close(output[0]);
		(void)close(output[1]);
		execl("/usr/bin/ffmpeg", "ffmpeg", "-loglevel", "error", "-threads", "1", "-fflags",
		      "nobuffer", "-flags", "low_delay", "-f", "h264", "-i", "pipe:0", "-an", "-pix_fmt",
		      "bgra", "-f", "rawvideo", "pipe:1", (char*)NULL);
		_exit(127);
	}
	(void)close(input[0]);
	(void)close(output[1]);
	decoder->input = input[1];
	decoder->output = output[0];
	if (!set_nonblocking(decoder->input) || !set_nonblocking(decoder->output))
		return false;
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
	return false;
}

static bool decoder_drain(FfmpegDecoder* decoder, int timeout_ms)
{
	const uint64_t frame_bytes = (uint64_t)WIDTH * HEIGHT * 4U;
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
			decoder->output_bytes += (uint64_t)length;
			while (decoder->output_bytes >= frame_bytes)
			{
				decoder->output_bytes -= frame_bytes;
				decoder->frames++;
				fprintf(stderr, "decoded=%u raw_bgra=%ux%u\n", decoder->frames, WIDTH, HEIGHT);
			}
			continue;
		}
		if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return true;
		return length == 0;
	}
}

static bool decoder_write(FfmpegDecoder* decoder, const uint8_t* data, size_t length)
{
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

static void decoder_stop(FfmpegDecoder* decoder)
{
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
}

int main(void)
{
	FoldVncClient foldvnc = { .fd = -1 };
	FfmpegDecoder decoder = { .pid = -1, .input = -1, .output = -1 };
	const uint64_t deadline = monotonic_milliseconds() + (PROBE_SECONDS * 1000U);
	uint32_t submitted = 0;
	int result = EXIT_FAILURE;

	(void)signal(SIGINT, on_signal);
	(void)signal(SIGTERM, on_signal);
	(void)signal(SIGPIPE, SIG_IGN);
	if (!foldvnc_client_connect(&foldvnc, "127.0.0.1", 7890, WIDTH, HEIGHT, 60) ||
	    !decoder_start(&decoder))
		goto out;

	fprintf(stderr, "FoldVNC ffmpeg decode probe started: %ux%u for %u seconds\n", WIDTH, HEIGHT,
	        PROBE_SECONDS);
	while (!stop_requested && monotonic_milliseconds() < deadline)
	{
		uint8_t* data = NULL;
		size_t length = 0;
		bool keyframe = false;
		if (!foldvnc_client_read_video(&foldvnc, &data, &length, &keyframe) || !data || length == 0)
		{
			fprintf(stderr, "FoldVNC video read failed\n");
			goto out;
		}

		if (!decoder_write(&decoder, data, length))
		{
			fprintf(stderr, "ffmpeg input pipe failed for FoldVNC H.264 frame\n");
			free(data);
			goto out;
		}
		if (keyframe)
			fprintf(stderr, "FoldVNC keyframe submitted bytes=%zu\n", length);
		submitted++;
		free(data);
		(void)decoder_drain(&decoder, 0);
	}
	fprintf(stderr, "FoldVNC ffmpeg decode probe complete: submitted=%u decoded=%u\n", submitted,
	        decoder.frames);
	result = decoder.frames > 0 ? EXIT_SUCCESS : EXIT_FAILURE;

out:
	decoder_stop(&decoder);
	foldvnc_client_close(&foldvnc);
	return result;
}
