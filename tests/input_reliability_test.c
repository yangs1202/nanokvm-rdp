#include "hid.h"
#include "protocol.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct { HidState hid; int receiver[3]; } Fixture;
static uint64_t now_ms(void)
{
	struct timespec t;
	assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
	return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static void setup(Fixture* f)
{
	hid_init(&f->hid, "/nonexistent/keyboard", "/nonexistent/mouse", "/nonexistent/touch");
	int* const slots[] = { &f->hid.keyboard_fd, &f->hid.mouse_fd, &f->hid.touch_fd };
	for (unsigned i = 0; i < 3; i++)
	{
		int pair[2];
		assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
		assert(fcntl(pair[0], F_SETFL, O_NONBLOCK) == 0);
		assert(fcntl(pair[1], F_SETFL, O_NONBLOCK) == 0);
		*slots[i] = pair[0];
		f->receiver[i] = pair[1];
	}
}
static void cleanup(Fixture* f)
{
	close(f->hid.keyboard_fd); close(f->hid.mouse_fd); close(f->hid.touch_fd);
	for (unsigned i = 0; i < 3; i++) close(f->receiver[i]);
}
static void block_fd(int fd)
{
	uint8_t data[8] = {0};
	while (write(fd, data, sizeof(data)) > 0) {}
	assert(errno == EAGAIN || errno == EWOULDBLOCK);
}
static void drain(int fd)
{
	uint8_t data[4096];
	while (read(fd, data, sizeof(data)) > 0) {}
	assert(errno == EAGAIN || errno == EWOULDBLOCK);
}
static void read_exact(int fd, void* data, size_t length)
{
	size_t offset = 0;
	while (offset < length)
	{
		ssize_t n = read(fd, (uint8_t*)data + offset, length - offset);
		assert(n > 0);
		offset += (size_t)n;
	}
}
static void empty(int fd)
{
	uint8_t data;
	assert(read(fd, &data, 1) == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
}
static void test_button_mapping_and_mixed_motion(void)
{
	Fixture f; setup(&f);
	uint8_t buttons = hid_pointer_buttons(0, 0xa000);
	assert(buttons == 2);
	assert(hid_absolute(&f.hid, 200, 300, 1920, 1080, 0xa000));
	uint8_t touch[6]; read_exact(f.receiver[2], touch, 6); assert(touch[0] == 2);
	empty(f.receiver[1]);
	buttons = hid_pointer_buttons(buttons, 0x0800); /* move after absolute down */
	assert(hid_relative(&f.hid, 300, -270, buttons));
	uint8_t moves[3][4]; read_exact(f.receiver[1], moves, sizeof(moves));
	int x = 0, y = 0;
	for (unsigned i = 0; i < 3; i++)
	{
		assert(moves[i][0] == 0); x += (int8_t)moves[i][1]; y += (int8_t)moves[i][2];
	}
	assert(x == 300 && y == -270 && f.hid.buttons == 2);
	empty(f.receiver[2]);
	usleep(400000); /* Legitimate hold/drag must not expire after 350 ms. */
	assert(hid_flush(&f.hid)); empty(f.receiver[2]); assert(f.hid.buttons == 2);
	buttons = hid_pointer_buttons(buttons, 0x2000);
	assert(hid_relative(&f.hid, 0, 0, buttons));
	read_exact(f.receiver[2], touch, 6); assert(touch[0] == 0);
	const uint16_t flags[] = {0x9000, 0x1000, 0xc000, 0x4000, 0x8001, 1, 0x8002, 2};
	const uint8_t expected[] = {1, 0, 4, 0, 8, 0, 16, 0};
	for (unsigned i = 0; i < 8; i++)
	{
		assert(hid_absolute(&f.hid, 200, 300, 1920, 1080, flags[i]));
		read_exact(f.receiver[2], touch, 6); assert(touch[0] == expected[i]);
	}
	cleanup(&f);
}
static void test_final_release_and_coalescing(void)
{
	Fixture f; setup(&f);
	for (unsigned length = 6; length <= 7; length++)
	{
		f.hid.absolute_report_length = length;
		block_fd(f.hid.touch_fd);
		assert(hid_absolute(&f.hid, 1, 1, 1920, 1080, 0xa000));
		for (unsigned x = 2; x < 2000; x++)
			assert(hid_absolute(&f.hid, x, 20, 1920, 1080, 0x0800));
		assert(hid_absolute(&f.hid, 1919, 20, 1920, 1080, 0x2000));
		assert(f.hid.touch_queue.count == 3);
		drain(f.receiver[2]); assert(hid_flush(&f.hid));
		uint8_t report[7];
		read_exact(f.receiver[2], report, length); assert(report[0] == 2);
		read_exact(f.receiver[2], report, length); assert(report[0] == 2 && report[1] == 255 && report[2] == 127);
		read_exact(f.receiver[2], report, length); assert(report[0] == 0);
		empty(f.receiver[2]); empty(f.receiver[1]); assert(!hid_pending(&f.hid));
		/* Release can also block after the press was already delivered. */
		assert(hid_absolute(&f.hid, 100, 100, 1920, 1080, 0xa000));
		read_exact(f.receiver[2], report, length); assert(report[0] == 2);
		block_fd(f.hid.touch_fd);
		assert(hid_absolute(&f.hid, 100, 100, 1920, 1080, 0x2000));
		drain(f.receiver[2]); assert(hid_flush(&f.hid));
		read_exact(f.receiver[2], report, length); assert(report[0] == 0);
	}
	cleanup(&f);
}
static void test_keyboard_order_and_wrap(void)
{
	Fixture f; setup(&f);
	for (unsigned round = 0; round < 8; round++)
	{
		block_fd(f.hid.keyboard_fd);
		for (unsigned i = 0; i < 50; i++)
		{
			assert(hid_scancode(&f.hid, 0x1d, false, false));
			assert(hid_scancode(&f.hid, 0x1e, false, false));
			assert(hid_scancode(&f.hid, 0x1e, false, false)); /* duplicate repeat */
			assert(hid_scancode(&f.hid, 0x1e, false, true));
			assert(hid_scancode(&f.hid, 0x1d, false, true));
		}
		assert(f.hid.keyboard_queue.count == 200);
		drain(f.receiver[0]); assert(hid_flush(&f.hid));
		for (unsigned i = 0; i < 50; i++)
		{
			uint8_t reports[4][8]; read_exact(f.receiver[0], reports, sizeof(reports));
			assert(reports[0][0] == 1 && reports[0][2] == 0);
			assert(reports[1][0] == 1 && reports[1][2] == 4);
			assert(reports[2][0] == 1 && reports[2][2] == 0);
			assert(reports[3][0] == 0 && reports[3][2] == 0);
		}
		assert(!hid_pending(&f.hid));
	}
	cleanup(&f);
}
static void test_cancel_retries_all_endpoints(void)
{
	Fixture f; setup(&f);
	block_fd(f.hid.keyboard_fd); block_fd(f.hid.mouse_fd); block_fd(f.hid.touch_fd);
	assert(hid_scancode(&f.hid, 0x1e, false, false));
	assert(hid_absolute(&f.hid, 200, 300, 1920, 1080, 0xa000));
	hid_release_all(&f.hid);
	assert(f.hid.keyboard_queue.count == 1 && f.hid.mouse_queue.count == 1 && f.hid.touch_queue.count == 1);
	for (unsigned i = 0; i < 3; i++) drain(f.receiver[i]);
	struct pollfd fds[3]; assert(hid_pollfds(&f.hid, fds) == 3);
	assert(poll(fds, 3, 10) == 3); assert(hid_flush(&f.hid));
	uint8_t keyboard[8], mouse[4], touch[6];
	read_exact(f.receiver[0], keyboard, 8); read_exact(f.receiver[1], mouse, 4); read_exact(f.receiver[2], touch, 6);
	for (unsigned i = 0; i < 8; i++) assert(keyboard[i] == 0);
	for (unsigned i = 0; i < 4; i++) assert(mouse[i] == 0);
	assert(touch[0] == 0 && !hid_pending(&f.hid));
	cleanup(&f);
}
static void test_overflow_and_stall(void)
{
	Fixture f; setup(&f); block_fd(f.hid.keyboard_fd);
	for (unsigned i = 0; i < HID_REPORT_QUEUE_CAPACITY; i++)
		assert(hid_scancode(&f.hid, 0x1e, false, (i % 2) != 0));
	assert(!hid_scancode(&f.hid, 0x1e, false, false));
	assert(f.hid.queue_overflows == 1);
	hid_release_all(&f.hid); /* Same cancellation used by the agent on failure. */
	f.hid.keyboard_queue.blocked_at = now_ms() - HID_REPORT_STALL_TIMEOUT_MS;
	assert(!hid_flush(&f.hid));
	hid_release_all(&f.hid);
	drain(f.receiver[0]); assert(hid_flush(&f.hid));
	uint8_t report[8]; read_exact(f.receiver[0], report, 8); assert(report[0] == 0 && report[2] == 0);
	empty(f.receiver[0]); cleanup(&f);
}
static void test_endpoint_reopen(void)
{
	char path[] = "/tmp/nanokvm-reopen-XXXXXX";
	int temp = mkstemp(path); assert(temp >= 0); close(temp); unlink(path);
	assert(mkfifo(path, 0600) == 0);
	HidState hid; hid_init(&hid, path, "/dev/null", "/dev/null");
	assert(!hid_scancode(&hid, 0x1e, false, false));
	hid_release_all(&hid);
	int receiver = open(path, O_RDONLY | O_NONBLOCK); assert(receiver >= 0);
	const uint64_t deadline = now_ms() + 500;
	while (hid_pending(&hid) && now_ms() < deadline)
	{
		assert(hid_flush(&hid)); poll(NULL, 0, 1);
	}
	assert(!hid_pending(&hid) && hid.keyboard_fd >= 0);
	uint8_t report[8]; read_exact(receiver, report, 8);
	for (unsigned i = 0; i < 8; i++) assert(report[i] == 0);
	empty(receiver); /* Failed/stale press must not be replayed after recovery. */
	assert(hid_scancode(&hid, 0x30, false, false));
	assert(hid_scancode(&hid, 0x30, false, true));
	read_exact(receiver, report, 8); assert(report[2] == 5);
	read_exact(receiver, report, 8); assert(report[2] == 0);
	close(receiver); close(hid.keyboard_fd); close(hid.mouse_fd); close(hid.touch_fd); unlink(path);
}
static void test_text_does_not_block_click_release(void)
{
	Fixture f; setup(&f);
	assert(hid_absolute(&f.hid, 100, 100, 1920, 1080, 0xa000));
	uint8_t report[6]; read_exact(f.receiver[2], report, 6);
	uint64_t start = now_ms();
	const uint8_t text[] = "한글";
	assert(hid_type_utf8(&f.hid, text, sizeof(text) - 1));
	assert(now_ms() - start < 100); /* Old synchronous path takes >=240 ms. */
	assert(hid_keyboard_pending(&f.hid));
	assert(hid_absolute(&f.hid, 100, 100, 1920, 1080, 0x2000));
	read_exact(f.receiver[2], report, 6); assert(report[0] == 0);
	while (hid_pending(&f.hid) && now_ms() - start < 1000)
	{
		assert(hid_flush(&f.hid));
		poll(NULL, 0, hid_poll_timeout(&f.hid, 20));
	}
	assert(!hid_pending(&f.hid));
	uint8_t keys[13][8]; read_exact(f.receiver[0], keys, sizeof(keys));
	assert(keys[0][2] == 0 && keys[1][2] == 0x0a && keys[12][2] == 0);
	cleanup(&f);
}
static void test_paste_order(void)
{
	Fixture f; setup(&f);
	int paste[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, paste) == 0);
	assert(fcntl(paste[1], F_SETFL, O_NONBLOCK) == 0);
	f.hid.paste_fd = paste[0];
	const uint8_t text[] = "🙂🙃";
	assert(hid_type_utf8(&f.hid, text, sizeof(text) - 1));
	empty(paste[1]);
	unsigned clipboard = 0, down = 0, up = 0;
	const uint64_t deadline = now_ms() + 1000;
	while (now_ms() < deadline)
	{
		assert(hid_flush(&f.hid));
		char content[7];
		const ssize_t n = read(paste[1], content, sizeof(content));
		if (n > 0)
		{
			assert(n == 7 && clipboard == up);
			assert(memcmp(content, clipboard == 0 ? "'🙂'\n" : "'🙃'\n", 7) == 0);
			clipboard++;
		}
		uint8_t report[8];
		while (read(f.receiver[0], report, 8) == 8)
		{
			if (report[2] == 0x19)
			{
				down++; assert(report[0] == 0x08 && clipboard == down);
			}
			else if (down > up)
			{
				up++; assert(report[0] == 0 && report[2] == 0);
			}
		}
		if (!hid_pending(&f.hid)) break;
		poll(NULL, 0, 1);
	}
	assert(clipboard == 2 && down == 2 && up == 2 && !hid_pending(&f.hid));
	close(paste[0]); close(paste[1]); cleanup(&f);
}
static void test_wheel(void)
{
	Fixture f; setup(&f);
	f.hid.absolute_report_length = 7;
	const uint16_t flags[] = {0x0278, 0x0388, 0x0478, 0x0588, 0x02f0};
	const int8_t vertical[] = {1, -1, 0, 0, 2};
	const int8_t horizontal[] = {0, 0, 1, -1, 0};
	for (unsigned i = 0; i < 5; i++)
	{
		assert(hid_wheel(&f.hid, flags[i]));
		uint8_t report[7]; read_exact(f.receiver[2], report, 7);
		assert((int8_t)report[5] == vertical[i] && (int8_t)report[6] == horizontal[i]);
		empty(f.receiver[2]);
	}
	f.hid.absolute_report_length = 6;
	assert(hid_wheel(&f.hid, 0x0478)); empty(f.receiver[2]);
	cleanup(&f);
}
static void test_fragmented_network_with_pending_release(void)
{
	int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	NanokvmControlReader reader = {0}; NanokvmControlMessage message = {0};
	const uint8_t frame[] = {NANOKVM_PROTOCOL_VERSION, NANOKVM_CONTROL_KEY, 0, 3, 0x1e, 0, 1};
	Fixture f; setup(&f); block_fd(f.hid.keyboard_fd); hid_release_all(&f.hid);
	for (unsigned i = 0; i < sizeof(frame); i++)
	{
		assert(write(sockets[0], frame + i, 1) == 1);
		const uint64_t start = now_ms();
		assert(protocol_receive_available(sockets[1], &reader, &message) == (i == sizeof(frame) - 1 ? 1 : 0));
		assert(now_ms() - start < 100);
		if (i == 4) /* A partial payload must not prevent final key release. */
		{
			drain(f.receiver[0]); assert(hid_flush(&f.hid));
			uint8_t keys[8]; read_exact(f.receiver[0], keys, 8); assert(keys[2] == 0);
		}
	}
	assert(message.type == NANOKVM_CONTROL_KEY && message.length == 3 && message.payload[2] == 1);
	assert(protocol_send(sockets[0], NANOKVM_CONTROL_PING, NULL, 0));
	assert(protocol_send(sockets[0], NANOKVM_CONTROL_KEY, frame + 4, 3));
	assert(protocol_receive_available(sockets[1], &reader, &message) == 1 && message.length == 0);
	assert(protocol_receive_available(sockets[1], &reader, &message) == 1 && message.length == 3);
	assert(protocol_receive_available(sockets[1], &reader, &message) == 0);
	uint8_t oversized[] = {NANOKVM_PROTOCOL_VERSION, 1, 0xff, 0xff};
	assert(write(sockets[0], oversized, 4) == 4);
	assert(protocol_receive_available(sockets[1], &reader, &message) == -1);
	reader = (NanokvmControlReader){0}; close(sockets[0]);
	assert(protocol_receive_available(sockets[1], &reader, &message) == -1);
	close(sockets[1]); cleanup(&f);
}
int main(void)
{
	test_button_mapping_and_mixed_motion(); test_final_release_and_coalescing();
	test_keyboard_order_and_wrap(); test_cancel_retries_all_endpoints();
	test_overflow_and_stall(); test_endpoint_reopen(); test_text_does_not_block_click_release();
	test_paste_order(); test_wheel(); test_fragmented_network_with_pending_release();
	puts("Input reliability: all 10 scenarios passed"); return 0;
}
