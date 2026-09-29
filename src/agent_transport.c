#include "agent_transport.h"

#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static uint64_t transport_now(AgentTransport* transport)
{
	return transport->callbacks.clock ? transport->callbacks.clock(transport->callbacks.context) : 0;
}

static void cancel_key_ack_locked(AgentTransport* transport)
{
	transport->waiting_key_sequence = 0;
	transport->key_ack_received = false;
	transport->key_ack_success = false;
}

static bool is_current_locked(const AgentTransport* transport, int fd, uint64_t epoch)
{
	return !transport->stopping && transport->fd == fd && transport->epoch == epoch;
}

static bool send_for_epoch(AgentTransport* transport, int fd, uint64_t epoch, uint8_t type,
                           const void* payload, uint16_t length)
{
	int send_fd = -1;
	(void)pthread_mutex_lock(&transport->send_lock);
	(void)pthread_mutex_lock(&transport->lock);
	if (is_current_locked(transport, fd, epoch))
		send_fd = dup(fd);
	(void)pthread_mutex_unlock(&transport->lock);
	bool sent = false;
	if (send_fd >= 0)
	{
		if (transport->callbacks.send_started)
			transport->callbacks.send_started(transport->callbacks.context, send_fd, epoch);
		sent = protocol_send(send_fd, type, payload, length);
	}
	if (send_fd >= 0)
		(void)close(send_fd);
	(void)pthread_mutex_unlock(&transport->send_lock);
	return sent;
}

static void publish_event(AgentTransport* transport, AgentTransportEvent event, uint64_t epoch)
{
	if (transport->callbacks.event)
		transport->callbacks.event(transport->callbacks.context, event, epoch);
}

bool agent_transport_init(AgentTransport* transport, const AgentTransportCallbacks* callbacks)
{
	if (!transport)
		return false;
	memset(transport, 0, sizeof(*transport));
	transport->fd = -1;
	transport->pending_fd = -1;
	if (callbacks)
		transport->callbacks = *callbacks;
	if (pthread_mutex_init(&transport->lock, NULL) != 0)
		return false;
	if (pthread_mutex_init(&transport->send_lock, NULL) != 0)
	{
		(void)pthread_mutex_destroy(&transport->lock);
		return false;
	}
	return true;
}

void agent_transport_destroy(AgentTransport* transport)
{
	if (!transport)
		return;
	int fd = -1;
	int pending_fd = -1;
	(void)pthread_mutex_lock(&transport->lock);
	fd = transport->fd;
	pending_fd = transport->pending_fd;
	transport->fd = -1;
	transport->pending_fd = -1;
	transport->stopping = true;
	(void)pthread_mutex_unlock(&transport->lock);
	if (fd >= 0)
		(void)shutdown(fd, SHUT_RDWR);
	if (pending_fd >= 0 && pending_fd != fd)
		(void)shutdown(pending_fd, SHUT_RDWR);
	(void)pthread_mutex_lock(&transport->send_lock);
	if (fd >= 0)
		(void)close(fd);
	if (pending_fd >= 0 && pending_fd != fd)
		(void)close(pending_fd);
	(void)pthread_mutex_unlock(&transport->send_lock);
	(void)pthread_mutex_destroy(&transport->send_lock);
	(void)pthread_mutex_destroy(&transport->lock);
}

bool agent_transport_accept(AgentTransport* transport, int fd, uint64_t* epoch)
{
	NanokvmControlMessage hello = { 0 };
	if (!transport || fd < 0)
	{
		if (fd >= 0)
			(void)close(fd);
		return false;
	}
	(void)pthread_mutex_lock(&transport->lock);
	const bool can_wait = !transport->stopping && transport->pending_fd < 0;
	if (can_wait)
		transport->pending_fd = fd;
	(void)pthread_mutex_unlock(&transport->lock);
	if (!can_wait)
	{
		(void)close(fd);
		return false;
	}
	const struct timeval receive_timeout = {
		.tv_sec = AGENT_TRANSPORT_HANDSHAKE_TIMEOUT_MS / 1000U,
		.tv_usec = (AGENT_TRANSPORT_HANDSHAKE_TIMEOUT_MS % 1000U) * 1000U,
	};
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
	const bool valid_hello = protocol_receive(fd, &hello) &&
	                         hello.type == NANOKVM_CONTROL_HELLO &&
	                         (hello.length == NANOKVM_HELLO_BASE_PAYLOAD_SIZE ||
	                          hello.length == NANOKVM_HELLO_CAPABILITIES_PAYLOAD_SIZE);
	const struct timeval blocking_receive = { 0 };
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &blocking_receive, sizeof(blocking_receive));

	int replaced_fd = -1;
	uint64_t accepted_epoch = 0;
	bool send_start = false;
	bool accepted = false;
	(void)pthread_mutex_lock(&transport->lock);
	if (transport->pending_fd == fd)
		transport->pending_fd = -1;
	if (valid_hello && !transport->stopping)
	{
		replaced_fd = transport->fd;
		transport->fd = fd;
		transport->epoch++;
		transport->supports_key_ack =
		    hello.length == NANOKVM_HELLO_CAPABILITIES_PAYLOAD_SIZE &&
		    (hello.payload[8] & NANOKVM_AGENT_CAPABILITY_KEY_ACK) != 0;
		transport->last_activity_at = transport_now(transport);
		transport->last_ping_at = transport->last_activity_at;
		transport->heartbeat_timed_out = false;
		cancel_key_ack_locked(transport);
		send_start = transport->stream_requested;
		accepted_epoch = transport->epoch;
		if (epoch)
			*epoch = accepted_epoch;
		accepted = true;
	}
	(void)pthread_mutex_unlock(&transport->lock);

	if (!accepted)
	{
		(void)close(fd);
		return false;
	}
	if (replaced_fd >= 0)
		(void)shutdown(replaced_fd, SHUT_RDWR);
	if (send_start)
		(void)send_for_epoch(transport, fd, accepted_epoch, NANOKVM_CONTROL_START_STREAM, NULL, 0);
	return true;
}

static void handle_message(AgentTransport* transport, int fd, uint64_t epoch,
	                       const NanokvmControlMessage* message)
{
	bool send_pong = false;
	(void)pthread_mutex_lock(&transport->lock);
	if (is_current_locked(transport, fd, epoch))
	{
		transport->last_activity_at = transport_now(transport);
		if (message->type == NANOKVM_CONTROL_KEY_ACK &&
		    message->length == NANOKVM_KEY_ACK_PAYLOAD_SIZE)
		{
			const uint32_t sequence = protocol_read_u32(message->payload);
			if (transport->waiting_key_sequence == sequence)
			{
				transport->key_ack_received = true;
				transport->key_ack_success = message->payload[4] != 0;
			}
		}
		else if (message->type == NANOKVM_CONTROL_STATS &&
		         message->length == NANOKVM_STATS_PAYLOAD_SIZE)
		{
			transport->stats.sent_packets = protocol_read_u32(message->payload);
			transport->stats.dropped_packets = protocol_read_u32(message->payload + 4);
			transport->stats.capture_frames = protocol_read_u32(message->payload + 8);
			transport->stats.dropped_frames = protocol_read_u32(message->payload + 12);
		}
		else if (message->type == NANOKVM_CONTROL_PING)
			send_pong = true;
	}
	(void)pthread_mutex_unlock(&transport->lock);
	if (send_pong)
		(void)send_for_epoch(transport, fd, epoch, NANOKVM_CONTROL_PONG, NULL, 0);
}

void agent_transport_run(AgentTransport* transport, int fd, uint64_t epoch)
{
	if (!transport || fd < 0)
		return;
	for (;;)
	{
		NanokvmControlMessage message = { 0 };
		if (!protocol_receive(fd, &message))
			break;
		handle_message(transport, fd, epoch, &message);
		(void)pthread_mutex_lock(&transport->lock);
		const bool current = is_current_locked(transport, fd, epoch);
		(void)pthread_mutex_unlock(&transport->lock);
		if (!current)
			break;
	}

	bool disconnected = false;
	(void)pthread_mutex_lock(&transport->lock);
	if (transport->fd == fd && transport->epoch == epoch)
	{
		transport->fd = -1;
		transport->supports_key_ack = false;
		cancel_key_ack_locked(transport);
		disconnected = !transport->stopping;
	}
	(void)pthread_mutex_unlock(&transport->lock);
	(void)close(fd);
	if (disconnected)
		publish_event(transport, AGENT_TRANSPORT_EVENT_DISCONNECTED, epoch);
}

void agent_transport_shutdown(AgentTransport* transport)
{
	if (!transport)
		return;
	(void)pthread_mutex_lock(&transport->lock);
	transport->stopping = true;
	if (transport->fd >= 0)
		(void)shutdown(transport->fd, SHUT_RDWR);
	if (transport->pending_fd >= 0)
		(void)shutdown(transport->pending_fd, SHUT_RDWR);
	(void)pthread_mutex_unlock(&transport->lock);
}

bool agent_transport_send(AgentTransport* transport, uint8_t type, const void* payload,
	                      uint16_t length)
{
	if (!transport)
		return false;
	int fd = -1;
	uint64_t epoch = 0;
	(void)pthread_mutex_lock(&transport->lock);
	if (!transport->stopping && transport->fd >= 0)
	{
		fd = transport->fd;
		epoch = transport->epoch;
	}
	(void)pthread_mutex_unlock(&transport->lock);
	return fd >= 0 && send_for_epoch(transport, fd, epoch, type, payload, length);
}

bool agent_transport_set_stream_requested(AgentTransport* transport, bool requested)
{
	if (!transport)
		return false;
	int fd = -1;
	uint64_t epoch = 0;
	(void)pthread_mutex_lock(&transport->lock);
	transport->stream_requested = requested;
	if (!transport->stopping && transport->fd >= 0)
	{
		fd = transport->fd;
		epoch = transport->epoch;
	}
	(void)pthread_mutex_unlock(&transport->lock);
	return fd >= 0 && send_for_epoch(transport, fd, epoch,
	                                 requested ? NANOKVM_CONTROL_START_STREAM
	                                           : NANOKVM_CONTROL_STOP_STREAM,
	                                 NULL, 0);
}

void agent_transport_heartbeat(AgentTransport* transport, uint64_t now)
{
	if (!transport)
		return;
	bool timeout = false;
	bool send_ping = false;
	int fd = -1;
	uint64_t epoch = 0;
	(void)pthread_mutex_lock(&transport->lock);
	if (!transport->stopping && transport->fd >= 0)
	{
		epoch = transport->epoch;
		timeout = !transport->heartbeat_timed_out &&
		          now - transport->last_activity_at > AGENT_TRANSPORT_HEARTBEAT_TIMEOUT_MS;
		if (timeout)
		{
			transport->heartbeat_timed_out = true;
			transport->stream_requested = false;
			(void)shutdown(transport->fd, SHUT_RDWR);
		}
		else if (!transport->heartbeat_timed_out &&
		         now - transport->last_ping_at >= AGENT_TRANSPORT_HEARTBEAT_INTERVAL_MS)
		{
			transport->last_ping_at = now;
			send_ping = true;
		}
		fd = transport->fd;
	}
	(void)pthread_mutex_unlock(&transport->lock);
	if (send_ping)
		(void)send_for_epoch(transport, fd, epoch, NANOKVM_CONTROL_PING, NULL, 0);
	if (timeout)
		publish_event(transport, AGENT_TRANSPORT_EVENT_HEARTBEAT_TIMEOUT, epoch);
}

bool agent_transport_is_current_epoch(AgentTransport* transport, uint64_t epoch)
{
	if (!transport)
		return false;
	(void)pthread_mutex_lock(&transport->lock);
	const bool current = !transport->stopping && transport->fd >= 0 && transport->epoch == epoch;
	(void)pthread_mutex_unlock(&transport->lock);
	return current;
}

uint64_t agent_transport_current_epoch(AgentTransport* transport)
{
	if (!transport)
		return 0;
	(void)pthread_mutex_lock(&transport->lock);
	const uint64_t epoch = !transport->stopping && transport->fd >= 0 ? transport->epoch : 0;
	(void)pthread_mutex_unlock(&transport->lock);
	return epoch;
}

void agent_transport_get_stats(AgentTransport* transport, AgentTransportStats* stats)
{
	if (!transport || !stats)
		return;
	(void)pthread_mutex_lock(&transport->lock);
	*stats = transport->stats;
	(void)pthread_mutex_unlock(&transport->lock);
}

bool agent_transport_supports_key_ack(AgentTransport* transport)
{
	if (!transport)
		return false;
	(void)pthread_mutex_lock(&transport->lock);
	const bool supported = transport->fd >= 0 && transport->supports_key_ack;
	(void)pthread_mutex_unlock(&transport->lock);
	return supported;
}

bool agent_transport_begin_key_ack(AgentTransport* transport, uint32_t sequence)
{
	if (!transport || sequence == 0)
		return false;
	(void)pthread_mutex_lock(&transport->lock);
	const bool started = !transport->stopping && transport->fd >= 0 &&
	                     transport->supports_key_ack;
	if (started)
	{
		transport->waiting_key_sequence = sequence;
		transport->key_ack_received = false;
		transport->key_ack_success = false;
	}
	(void)pthread_mutex_unlock(&transport->lock);
	return started;
}

bool agent_transport_get_key_ack(AgentTransport* transport, uint32_t sequence,
	                             bool* received, bool* success)
{
	if (!transport || !received || !success)
		return false;
	(void)pthread_mutex_lock(&transport->lock);
	const bool matches = sequence != 0 && transport->waiting_key_sequence == sequence;
	if (matches)
	{
		*received = transport->key_ack_received;
		*success = transport->key_ack_success;
	}
	(void)pthread_mutex_unlock(&transport->lock);
	return matches;
}
