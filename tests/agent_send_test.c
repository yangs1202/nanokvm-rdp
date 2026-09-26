/* Exercise the production sender with injected kernel errors. */
#define main agent_program_main
#define sendto test_sendto
#include "../src/agent.c"
#undef sendto
#undef main
#include <assert.h>

static unsigned calls;
static unsigned failures;
static int injected_error;
static const void* expected_packet;
static size_t expected_length;

ssize_t test_sendto(int fd, const void* data, size_t length, int flags,
                   const struct sockaddr* address, socklen_t address_length)
{
	(void)fd; (void)address; (void)address_length;
	assert(data == expected_packet && length == expected_length);
	assert(flags == MSG_DONTWAIT);
	calls++;
	if (calls <= failures) { errno = injected_error; return -1; }
	return (ssize_t)length;
}

int main(void)
{
	const uint8_t packet[] = { 1, 2, 3 };
	expected_packet = packet;
	expected_length = sizeof(packet);
	const int transient[] = { EAGAIN, ENOBUFS, EINTR };
	for (unsigned i = 0; i < sizeof(transient) / sizeof(transient[0]); i++)
	{
		Agent agent = { .send_deadline_ms = monotonic_milliseconds() + 40U };
		atomic_init(&agent.sent_packets, 0); atomic_init(&agent.dropped_packets, 0);
		calls = 0; failures = 2; injected_error = transient[i];
		assert(send_packet(&agent, packet, sizeof(packet)));
		assert(calls == 3 && agent.send_retries == 2);
		assert(atomic_load(&agent.sent_packets) == 1 && !atomic_load(&agent.dropped_packets));
	}
	Agent agent = { .send_deadline_ms = monotonic_milliseconds() + 3U };
	atomic_init(&agent.sent_packets, 0); atomic_init(&agent.dropped_packets, 0);
	calls = 0; failures = UINT32_MAX; injected_error = EAGAIN;
	assert(!send_packet(&agent, packet, sizeof(packet)));
	assert(agent.send_retries > 0 && atomic_load(&agent.dropped_packets) == 1);
	/* Fatal errors bypass retries, even with time remaining. */
	agent.send_deadline_ms = monotonic_milliseconds() + 40U;
	calls = 0; injected_error = EMSGSIZE;
	assert(!send_packet(&agent, packet, sizeof(packet)) && calls == 1);
	return 0;
}
