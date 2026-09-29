#include "agent_transport.h"

#include <assert.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct
{
	atomic_uint_fast64_t now;
	atomic_uint disconnects;
	atomic_uint timeouts;
	atomic_uint_fast64_t last_epoch;
} TestContext;

typedef struct
{
	AgentTransport* transport;
	int fd;
	uint64_t epoch;
} RunArgument;

typedef struct
{
	AgentTransport* transport;
	int fd;
	bool accepted;
} AcceptArgument;

typedef struct
{
	AgentTransport* transport;
	atomic_bool entered;
	atomic_bool finished;
} SendArgument;

static uint64_t test_clock(void* context)
{
	return atomic_load(&((TestContext*)context)->now);
}

static void test_event(void* context, AgentTransportEvent event, uint64_t epoch)
{
	TestContext* test = (TestContext*)context;
	atomic_store(&test->last_epoch, epoch);
	if (event == AGENT_TRANSPORT_EVENT_DISCONNECTED)
		(void)atomic_fetch_add(&test->disconnects, 1);
	else if (event == AGENT_TRANSPORT_EVENT_HEARTBEAT_TIMEOUT)
		(void)atomic_fetch_add(&test->timeouts, 1);
}

static void init_transport(AgentTransport* transport, TestContext* context)
{
	memset(context, 0, sizeof(*context));
	atomic_store(&context->now, 100);
	const AgentTransportCallbacks callbacks = {
		.event = test_event,
		.clock = test_clock,
		.context = context,
	};
	assert(agent_transport_init(transport, &callbacks));
}

static void send_hello(int fd, bool key_ack)
{
	uint8_t payload[NANOKVM_HELLO_CAPABILITIES_PAYLOAD_SIZE] = { 0 };
	payload[8] = key_ack ? NANOKVM_AGENT_CAPABILITY_KEY_ACK : 0;
	assert(protocol_send(fd, NANOKVM_CONTROL_HELLO, payload,
	                     key_ack ? NANOKVM_HELLO_CAPABILITIES_PAYLOAD_SIZE
	                             : NANOKVM_HELLO_BASE_PAYLOAD_SIZE));
}

static uint64_t accept_pair(AgentTransport* transport, int sockets[2], bool key_ack)
{
	uint64_t epoch = 0;
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	send_hello(sockets[1], key_ack);
	assert(agent_transport_accept(transport, sockets[0], &epoch));
	return epoch;
}

static void* run_connection(void* opaque)
{
	RunArgument* argument = (RunArgument*)opaque;
	agent_transport_run(argument->transport, argument->fd, argument->epoch);
	return NULL;
}

static void* accept_connection(void* opaque)
{
	AcceptArgument* argument = (AcceptArgument*)opaque;
	uint64_t epoch = 0;
	argument->accepted = agent_transport_accept(argument->transport, argument->fd, &epoch);
	return NULL;
}

static void* send_connection(void* opaque)
{
	SendArgument* argument = (SendArgument*)opaque;
	const uint8_t payload[NANOKVM_CONTROL_MAX_PAYLOAD] = { 0 };
	atomic_store(&argument->entered, true);
	(void)agent_transport_send(argument->transport, NANOKVM_CONTROL_KEY, payload, sizeof(payload));
	atomic_store(&argument->finished, true);
	return NULL;
}

static void wait_for_ack(AgentTransport* transport, uint32_t sequence, bool expected_success)
{
	for (unsigned attempt = 0; attempt < 1000; attempt++)
	{
		bool received = false;
		bool success = false;
		assert(agent_transport_get_key_ack(transport, sequence, &received, &success));
		if (received)
		{
			assert(success == expected_success);
			return;
		}
		(void)usleep(1000);
	}
	assert(!"timed out waiting for KEY_ACK");
}

static void test_send_preserves_v1_frame(void)
{
	AgentTransport transport;
	TestContext context;
	int sockets[2];
	const uint8_t payload[] = { 0x1d, 1, 0 };
	const uint8_t expected[] = { NANOKVM_PROTOCOL_VERSION, NANOKVM_CONTROL_KEY, 0, 3, 0x1d, 1, 0 };
	uint8_t frame[sizeof(expected)] = { 0 };

	init_transport(&transport, &context);
	(void)accept_pair(&transport, sockets, false);
	assert(agent_transport_send(&transport, NANOKVM_CONTROL_KEY, payload, sizeof(payload)));
	assert(read(sockets[1], frame, sizeof(frame)) == (ssize_t)sizeof(frame));
	assert(memcmp(frame, expected, sizeof(expected)) == 0);
	agent_transport_shutdown(&transport);
	assert(close(sockets[1]) == 0);
	agent_transport_destroy(&transport);
}

static void test_invalid_hello_is_rejected_and_closed(void)
{
	AgentTransport transport;
	TestContext context;
	int sockets[2];
	uint64_t epoch = 0;
	const uint8_t invalid[] = { NANOKVM_PROTOCOL_VERSION, NANOKVM_CONTROL_PING, 0, 0 };

	init_transport(&transport, &context);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	assert(write(sockets[1], invalid, sizeof(invalid)) == (ssize_t)sizeof(invalid));
	assert(!agent_transport_accept(&transport, sockets[0], &epoch));
	assert(read(sockets[1], (uint8_t[1]){ 0 }, 1) == 0);
	assert(close(sockets[1]) == 0);
	agent_transport_destroy(&transport);
}

static void test_pending_handshake_times_out_and_shutdown_unblocks(void)
{
	AgentTransport transport;
	TestContext context;
	int timed_out[2];
	uint64_t epoch = 0;

	init_transport(&transport, &context);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, timed_out) == 0);
	assert(!agent_transport_accept(&transport, timed_out[0], &epoch));
	assert(read(timed_out[1], (uint8_t[1]){ 0 }, 1) == 0);
	assert(close(timed_out[1]) == 0);
	agent_transport_destroy(&transport);

	int interrupted[2];
	init_transport(&transport, &context);
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, interrupted) == 0);
	AcceptArgument argument = { .transport = &transport, .fd = interrupted[0] };
	pthread_t thread;
	assert(pthread_create(&thread, NULL, accept_connection, &argument) == 0);
	(void)usleep(10000);
	agent_transport_shutdown(&transport);
	assert(pthread_join(thread, NULL) == 0);
	assert(!argument.accepted);
	assert(close(interrupted[1]) == 0);
	agent_transport_destroy(&transport);
}

static void test_partial_ping_pong_and_stats(void)
{
	AgentTransport transport;
	TestContext context;
	int sockets[2];
	const uint8_t ping[] = { NANOKVM_PROTOCOL_VERSION, NANOKVM_CONTROL_PING, 0, 0 };
	uint8_t stats[NANOKVM_STATS_PAYLOAD_SIZE] = { 0 };
	NanokvmControlMessage response = { 0 };
	const uint64_t epoch = (init_transport(&transport, &context),
	                        accept_pair(&transport, sockets, true));
	RunArgument argument = { .transport = &transport, .fd = sockets[0], .epoch = epoch };
	pthread_t thread;
	assert(pthread_create(&thread, NULL, run_connection, &argument) == 0);

	assert(write(sockets[1], ping, 3) == 3);
	assert(recv(sockets[1], (uint8_t[1]){ 0 }, 1, MSG_DONTWAIT) < 0);
	assert(write(sockets[1], ping + 3, 1) == 1);
	assert(protocol_receive(sockets[1], &response));
	assert(response.type == NANOKVM_CONTROL_PONG && response.length == 0);

	protocol_write_u32(stats, 11);
	protocol_write_u32(stats + 4, 12);
	protocol_write_u32(stats + 8, 13);
	protocol_write_u32(stats + 12, 14);
	assert(protocol_send(sockets[1], NANOKVM_CONTROL_STATS, stats, sizeof(stats)));
	AgentTransportStats observed = { 0 };
	for (unsigned attempt = 0; attempt < 1000 && observed.dropped_frames != 14; attempt++)
	{
		agent_transport_get_stats(&transport, &observed);
		(void)usleep(1000);
	}
	assert(observed.sent_packets == 11 && observed.dropped_packets == 12);
	assert(observed.capture_frames == 13 && observed.dropped_frames == 14);

	assert(close(sockets[1]) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(atomic_load(&context.disconnects) == 1);
	agent_transport_destroy(&transport);
}

static void test_reconnect_restores_stream_and_rejects_stale_ack(void)
{
	AgentTransport transport;
	TestContext context;
	int first[2];
	int second[2];
	NanokvmControlMessage message = { 0 };
	uint8_t ack[NANOKVM_KEY_ACK_PAYLOAD_SIZE] = { 0 };

	init_transport(&transport, &context);
	assert(!agent_transport_set_stream_requested(&transport, true));
	const uint64_t first_epoch = accept_pair(&transport, first, true);
	assert(protocol_receive(first[1], &message));
	assert(message.type == NANOKVM_CONTROL_START_STREAM && message.length == 0);
	assert(agent_transport_supports_key_ack(&transport));
	assert(agent_transport_begin_key_ack(&transport, 42));
	protocol_write_u32(ack, 42);
	ack[4] = 1;
	assert(protocol_send(first[1], NANOKVM_CONTROL_KEY_ACK, ack, sizeof(ack)));

	const uint64_t second_epoch = accept_pair(&transport, second, true);
	assert(second_epoch == first_epoch + 1);
	assert(!agent_transport_is_current_epoch(&transport, first_epoch));
	assert(agent_transport_is_current_epoch(&transport, second_epoch));
	assert(protocol_receive(second[1], &message));
	assert(message.type == NANOKVM_CONTROL_START_STREAM && message.length == 0);
	assert(agent_transport_begin_key_ack(&transport, 42));
	RunArgument stale = { .transport = &transport, .fd = first[0], .epoch = first_epoch };
	pthread_t stale_thread;
	assert(pthread_create(&stale_thread, NULL, run_connection, &stale) == 0);
	assert(pthread_join(stale_thread, NULL) == 0);
	bool received = true;
	bool success = true;
	assert(agent_transport_get_key_ack(&transport, 42, &received, &success));
	assert(!received && !success);

	RunArgument current = { .transport = &transport, .fd = second[0], .epoch = second_epoch };
	pthread_t current_thread;
	assert(pthread_create(&current_thread, NULL, run_connection, &current) == 0);
	assert(protocol_send(second[1], NANOKVM_CONTROL_KEY_ACK, ack, sizeof(ack)));
	wait_for_ack(&transport, 42, true);
	assert(agent_transport_set_stream_requested(&transport, false));
	assert(protocol_receive(second[1], &message));
	assert(message.type == NANOKVM_CONTROL_STOP_STREAM && message.length == 0);
	assert(close(second[1]) == 0);
	assert(close(first[1]) == 0);
	assert(pthread_join(current_thread, NULL) == 0);
	agent_transport_destroy(&transport);
}

static void test_heartbeat_timeout_is_stale_after_disconnect(void)
{
	AgentTransport transport;
	TestContext context;
	int sockets[2];
	NanokvmControlMessage message = { 0 };

	init_transport(&transport, &context);
	const uint64_t epoch = accept_pair(&transport, sockets, false);
	agent_transport_heartbeat(&transport, 1100);
	assert(protocol_receive(sockets[1], &message));
	assert(message.type == NANOKVM_CONTROL_PING && message.length == 0);
	agent_transport_heartbeat(&transport, 5101);
	assert(atomic_load(&context.timeouts) == 1);
	assert(atomic_load(&context.last_epoch) == epoch);
	agent_transport_heartbeat(&transport, 6101);
	assert(atomic_load(&context.timeouts) == 1);
	assert(!agent_transport_send(&transport, NANOKVM_CONTROL_PING, NULL, 0));
	RunArgument argument = { .transport = &transport, .fd = sockets[0], .epoch = epoch };
	pthread_t thread;
	assert(pthread_create(&thread, NULL, run_connection, &argument) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(!agent_transport_is_current_epoch(&transport, epoch));
	assert(close(sockets[1]) == 0);
	int reconnected[2];
	const uint64_t reconnected_epoch = accept_pair(&transport, reconnected, false);
	assert(reconnected_epoch == epoch + 1);
	assert(recv(reconnected[1], (uint8_t[1]){ 0 }, 1, MSG_DONTWAIT) < 0);
	agent_transport_shutdown(&transport);
	assert(close(reconnected[1]) == 0);
	agent_transport_destroy(&transport);
}

static void test_reconnect_unblocks_stalled_send(void)
{
	AgentTransport transport;
	TestContext context;
	int first[2];
	int second[2];
	int send_buffer = 1024;

	init_transport(&transport, &context);
	const uint64_t first_epoch = accept_pair(&transport, first, false);
	assert(setsockopt(first[0], SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) == 0);
	RunArgument reader = { .transport = &transport, .fd = first[0], .epoch = first_epoch };
	pthread_t reader_thread;
	assert(pthread_create(&reader_thread, NULL, run_connection, &reader) == 0);
	SendArgument sender = { .transport = &transport };
	pthread_t sender_thread;
	assert(pthread_create(&sender_thread, NULL, send_connection, &sender) == 0);
	for (unsigned attempt = 0; attempt < 1000 && !atomic_load(&sender.entered); attempt++)
		(void)usleep(1000);
	assert(atomic_load(&sender.entered));
	(void)usleep(10000);
	assert(!atomic_load(&sender.finished));

	const uint64_t second_epoch = accept_pair(&transport, second, false);
	assert(second_epoch == first_epoch + 1);
	assert(pthread_join(sender_thread, NULL) == 0);
	assert(atomic_load(&sender.finished));
	assert(pthread_join(reader_thread, NULL) == 0);
	assert(!agent_transport_is_current_epoch(&transport, first_epoch));
	assert(agent_transport_is_current_epoch(&transport, second_epoch));
	assert(close(first[1]) == 0);
	agent_transport_shutdown(&transport);
	assert(close(second[1]) == 0);
	agent_transport_destroy(&transport);
}

static void test_shutdown_unblocks_reader_before_lock_destroy(void)
{
	AgentTransport transport;
	TestContext context;
	int sockets[2];
	const uint64_t epoch = (init_transport(&transport, &context),
	                        accept_pair(&transport, sockets, false));
	RunArgument argument = { .transport = &transport, .fd = sockets[0], .epoch = epoch };
	pthread_t thread;
	assert(pthread_create(&thread, NULL, run_connection, &argument) == 0);
	agent_transport_shutdown(&transport);
	assert(pthread_join(thread, NULL) == 0);
	assert(atomic_load(&context.disconnects) == 0);
	assert(close(sockets[1]) == 0);
	agent_transport_destroy(&transport);
}

int main(void)
{
	(void)signal(SIGPIPE, SIG_IGN);
	test_send_preserves_v1_frame();
	test_invalid_hello_is_rejected_and_closed();
	test_pending_handshake_times_out_and_shutdown_unblocks();
	test_partial_ping_pong_and_stats();
	test_reconnect_restores_stream_and_rejects_stale_ack();
	test_heartbeat_timeout_is_stale_after_disconnect();
	test_reconnect_unblocks_stalled_send();
	test_shutdown_unblocks_reader_before_lock_destroy();
	return 0;
}
