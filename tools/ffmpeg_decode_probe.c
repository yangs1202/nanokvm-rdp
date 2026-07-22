#include "ffmpeg_decoder.h"
#include "foldvnc_client.h"

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CAPTURE_WIDTH 1920U
#define CAPTURE_HEIGHT 1080U
#define OUTPUT_WIDTH 960U
#define OUTPUT_HEIGHT 540U
#define PROBE_SECONDS 12U

typedef struct
{
	uint32_t frames;
} ProbeState;

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

static bool on_frame(void* context, const uint8_t* bgra, size_t length)
{
	ProbeState* state = context;
	(void)bgra;
	if (length != (size_t)OUTPUT_WIDTH * OUTPUT_HEIGHT * 4U)
		return false;
	state->frames++;
	fprintf(stderr, "decoded=%u raw_bgra=%ux%u\n", state->frames, OUTPUT_WIDTH, OUTPUT_HEIGHT);
	return true;
}

int main(void)
{
	FoldVncClient foldvnc = { .fd = -1 };
	FfmpegDecoder decoder = { .pid = -1, .input = -1, .output = -1 };
	ProbeState state = { 0 };
	const uint64_t deadline = monotonic_milliseconds() + (PROBE_SECONDS * 1000U);
	uint32_t submitted = 0;
	int result = EXIT_FAILURE;

	(void)signal(SIGINT, on_signal);
	(void)signal(SIGTERM, on_signal);
	(void)signal(SIGPIPE, SIG_IGN);
	if (!foldvnc_client_connect(&foldvnc, "127.0.0.1", 7890, CAPTURE_WIDTH, CAPTURE_HEIGHT, 60) ||
	    !ffmpeg_decoder_start(&decoder, OUTPUT_WIDTH, OUTPUT_HEIGHT, on_frame, &state))
		goto out;

	fprintf(stderr, "FoldVNC %ux%u → FFmpeg %ux%u decode probe started for %u seconds\n",
	        CAPTURE_WIDTH, CAPTURE_HEIGHT, OUTPUT_WIDTH, OUTPUT_HEIGHT, PROBE_SECONDS);
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
		if (!ffmpeg_decoder_push(&decoder, data, length))
		{
			fprintf(stderr, "ffmpeg input pipe failed for FoldVNC H.264 frame\n");
			free(data);
			goto out;
		}
		if (keyframe)
			fprintf(stderr, "FoldVNC keyframe submitted bytes=%zu\n", length);
		submitted++;
		free(data);
	}
	fprintf(stderr, "FoldVNC ffmpeg decode probe complete: submitted=%u decoded=%u\n", submitted,
	        state.frames);
	result = state.frames > 0 ? EXIT_SUCCESS : EXIT_FAILURE;

out:
	ffmpeg_decoder_stop(&decoder);
	foldvnc_client_close(&foldvnc);
	return result;
}
