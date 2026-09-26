#include "hid.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

static void expect_empty(int fd)
{
	uint8_t byte;
	assert(read(fd, &byte, 1) == -1);
	assert(errno == EAGAIN || errno == EWOULDBLOCK);
}

static void expect_motion(int fd, int8_t dx)
{
	uint8_t report[4];
	assert(read(fd, report, sizeof(report)) == sizeof(report));
	assert(report[0] == 0 && (int8_t)report[1] == dx && report[2] == 0 && report[3] == 0);
	expect_empty(fd);
}

int main(void)
{
	HidState hid;
	hid_init(&hid, "/nonexistent/keyboard", "/nonexistent/mouse", "/nonexistent/touch");
	int pair[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	assert(fcntl(pair[0], F_SETFL, O_NONBLOCK) == 0);
	assert(fcntl(pair[1], F_SETFL, O_NONBLOCK) == 0);
	hid.mouse_fd = pair[0];

	/* No early movement, one unit at five minutes, then opposite direction. */
	uint64_t due = hid.keepalive_at;
	assert(hid_keepalive(&hid, due - 1)); expect_empty(pair[1]);
	assert(hid_keepalive(&hid, due)); expect_motion(pair[1], 1);
	assert(hid.keepalive_at == due + HID_KEEPALIVE_INTERVAL_MS);
	assert(hid_keepalive(&hid, due)); expect_empty(pair[1]);
	due = hid.keepalive_at;
	assert(hid_keepalive(&hid, due)); expect_motion(pair[1], -1);

	/* Never create a drag, modifier gesture, or overtake queued input. */
	due = hid.keepalive_at;
	const uint8_t buttons[] = { 1, 2, 4, 8, 16 };
	for (unsigned i = 0; i < sizeof(buttons); i++)
	{
		hid.buttons = buttons[i];
		assert(hid_keepalive(&hid, due)); expect_empty(pair[1]);
		assert(hid.buttons == buttons[i] && hid.keepalive_at == due);
	}
	hid.buttons = 0;
	hid.modifiers = 1;
	assert(hid_keepalive(&hid, due)); expect_empty(pair[1]);
	hid.modifiers = 0;
	hid.usages[4] = true;
	assert(hid_keepalive(&hid, due)); expect_empty(pair[1]);
	hid.usages[4] = false;
	hid.pointer_queue.count = 1;
	assert(hid_keepalive(&hid, due)); expect_empty(pair[1]);
	hid.pointer_queue.count = 0;
	hid.keyboard_queue.count = 1;
	assert(hid_keepalive(&hid, due)); expect_empty(pair[1]);
	hid.keyboard_queue.count = 0;
	assert(hid_keepalive(&hid, due)); expect_motion(pair[1], 1);

	/* A long pause sends only one report, with the next interval starting now. */
	due = hid.keepalive_at + 10U * HID_KEEPALIVE_INTERVAL_MS;
	assert(hid_keepalive(&hid, due)); expect_motion(pair[1], -1);
	assert(hid.keepalive_at == due + HID_KEEPALIVE_INTERVAL_MS);
	assert(hid_keepalive(&hid, due)); expect_empty(pair[1]);

	/* USB backpressure retains exactly one motion until it can be submitted. */
	uint8_t buffer[4096] = { 0 };
	while (write(pair[0], buffer, sizeof(buffer)) > 0) {}
	assert(errno == EAGAIN || errno == EWOULDBLOCK);
	due = hid.keepalive_at;
	assert(hid_keepalive(&hid, due));
	assert(hid.pointer_queue.count == 1);
	assert(hid_keepalive(&hid, due + HID_KEEPALIVE_INTERVAL_MS));
	assert(hid.pointer_queue.count == 1);
	while (read(pair[1], buffer, sizeof(buffer)) > 0) {}
	assert(hid_flush(&hid)); expect_motion(pair[1], 1);
	assert(!hid_pending(&hid));

	/* Missing host endpoint surfaces failure and does not flood retries. */
	close(pair[0]); close(pair[1]); hid.mouse_fd = -1;
	due = hid.keepalive_at;
	assert(!hid_keepalive(&hid, due));
	assert(hid.keepalive_at == due + HID_KEEPALIVE_INTERVAL_MS);
	assert(hid.keepalive_dx == -1);
	assert(hid_keepalive(&hid, due));
	assert(hid.pointer_queue.count == 1);
	puts("HID keepalive: timing, input safety, backpressure, and failure passed");
	return 0;
}
