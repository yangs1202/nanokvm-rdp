#include "ffmpeg_decoder.h"

#include <assert.h>
#include <stdatomic.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

static atomic_uint frames;
static uint8_t last_frame[320U * 180U * 4U];
static AVFrame* latest;

static bool on_raw(void* context, const AVFrame* frame)
{
	(void)context;
	AVFrame* retained = av_frame_clone(frame);
	if (!retained) return false;
	av_frame_free(&latest);
	latest = retained;
	return true;
}

static void check_geometry_changes(void)
{
	FfmpegConverter reused = { 0 };
	const int widths[] = { 64, 64, 32, 32, 64 };
	const int heights[] = { 32, 32, 64, 64, 64 };
	for (unsigned i = 0; i < 5; i++)
	{
		AVFrame* frame = av_frame_alloc();
		assert(frame);
		frame->format = AV_PIX_FMT_YUV420P;
		frame->width = widths[i];
		frame->height = heights[i];
		assert(av_frame_get_buffer(frame, 32) == 0);
		for (int plane = 0; plane < 3; plane++)
			for (int row = 0; row < (plane ? frame->height / 2 : frame->height); row++)
				memset(frame->data[plane] + row * frame->linesize[plane],
				       plane ? 128 : 32 + i * 40, plane ? frame->width / 2 : frame->width);
		FfmpegConverter fresh = { 0 };
		const unsigned output_width = i == 3 ? 120 : 96;
		assert(ffmpeg_converter_convert(&fresh, frame, output_width, 96));
		assert(ffmpeg_converter_convert(&reused, frame, output_width, 96));
		assert(memcmp(fresh.frame, reused.frame, fresh.frame_size) == 0);
		for (size_t pixel = 3; pixel < reused.frame_size; pixel += 4)
			assert(reused.frame[pixel] == 255);
		ffmpeg_converter_free(&fresh);
		av_frame_free(&frame);
	}
	ffmpeg_converter_free(&reused);
}

static double milliseconds(void)
{
	struct timespec now;
	assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	return now.tv_sec * 1000.0 + now.tv_nsec / 1000000.0;
}

static bool on_padded_frame(void* context, const uint8_t* data, size_t length)
{
	unsigned* count = context;
	assert(length == 320U * 240U * 4U);
	for (unsigned y = 0; y < 240; y++)
	{
		if (y >= 30 && y < 210) continue;
		for (unsigned x = 0; x < 320; x++)
		{
			const uint8_t* pixel = data + (y * 320 + x) * 4;
			assert(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255);
		}
	}
	(*count)++;
	return true;
}

static bool on_frame(void* context, const uint8_t* data, size_t length)
{
	(void)context;
	assert(data && length == 320U * 180U * 4U);
	memcpy(last_frame, data, length);
	atomic_fetch_add(&frames, 1);
	return true;
}

int main(void)
{
	/* Every complete AU must be delivered before the next push, including P-frames. */
	char path[] = "/tmp/nanokvm-latency-XXXXXX";
	const int fd = mkstemp(path);
	assert(fd >= 0);
	const pid_t pid = fork();
	assert(pid >= 0);
	if (pid == 0)
	{
		assert(dup2(fd, STDOUT_FILENO) >= 0);
		close(fd);
		execlp("ffmpeg", "ffmpeg", "-v", "error", "-f", "lavfi", "-i",
		       "testsrc2=size=320x180:rate=30", "-frames:v", "8", "-c:v", "libx264",
		       "-preset", "ultrafast", "-tune", "zerolatency", "-x264-params",
		       "keyint=30:aud=1", "-f", "h264", "pipe:1", (char*)NULL);
		_exit(127);
	}
	int status = 0;
	assert(waitpid(pid, &status, 0) == pid);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	const off_t size = lseek(fd, 0, SEEK_END);
	assert(size > 0);
	assert(lseek(fd, 0, SEEK_SET) == 0);
	uint8_t* stream = malloc((size_t)size);
	assert(stream);
	size_t used = 0;
	while (used < (size_t)size)
	{
		const ssize_t n = read(fd, stream + used, (size_t)size - used);
		assert(n > 0);
		used += (size_t)n;
	}
	close(fd);
	unlink(path);
	(void)signal(SIGPIPE, SIG_IGN);
	atomic_init(&frames, 0);
	FfmpegDecoder decoder;
	assert(ffmpeg_decoder_start(&decoder, 320, 180, on_frame, NULL));
	FfmpegDecoder raw;
	assert(ffmpeg_decoder_start_raw(&raw, on_raw, NULL));
	FfmpegDecoder padded;
	unsigned padded_frames = 0;
	assert(ffmpeg_decoder_start(&padded, 320, 240, on_padded_frame, &padded_frames));
	double total_ms = 0;
	size_t start = 0;
	unsigned submitted = 0;
	for (size_t i = 5; i <= used; i++)
	{
		if (i != used && (i + 5 > used ||
		    memcmp(stream + i, "\x00\x00\x00\x01\x09", 5) != 0))
			continue;
		const double begin = milliseconds();
		assert(ffmpeg_decoder_push(&decoder, stream + start, i - start));
		total_ms += milliseconds() - begin;
		assert(ffmpeg_decoder_push(&padded, stream + start, i - start));
		assert(ffmpeg_decoder_push(&raw, stream + start, i - start));
		assert(raw.converter.frame == NULL); /* no eager BGRA allocation or conversion */
		start = i;
		submitted++;
		assert(atomic_load(&frames) == submitted);
		const struct timespec interval = { .tv_nsec = 40000000 };
		nanosleep(&interval, NULL);
	}
	free(stream);
	/* All P-frames were decoded, but only the last image will be converted.
	 * It must survive decoder shutdown and exactly match eager conversion. */
	ffmpeg_decoder_stop(&raw);
	FfmpegConverter deferred = { 0 };
	assert(latest);
	assert(ffmpeg_converter_convert(&deferred, latest, 320, 180));
	assert(memcmp(last_frame, deferred.frame, sizeof(last_frame)) == 0);
	av_frame_free(&latest);
	ffmpeg_converter_free(&deferred);
	check_geometry_changes();
	const unsigned before_eof = atomic_load(&frames);
	ffmpeg_decoder_stop(&decoder);
	ffmpeg_decoder_stop(&padded);
	assert(padded_frames == 8);
	fprintf(stderr, "average decode + BGRA conversion: %.2f ms\n", total_ms / submitted);
	fprintf(stderr, "decoded before EOF: %u (expected 8)\n", before_eof);
	return before_eof == 8 ? EXIT_SUCCESS : EXIT_FAILURE;
}
