#ifndef NANOKVM_RDP_AGENT_TRANSPORT_H
#define NANOKVM_RDP_AGENT_TRANSPORT_H

#include "protocol.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#define AGENT_TRANSPORT_HEARTBEAT_INTERVAL_MS 1000U
#define AGENT_TRANSPORT_HEARTBEAT_TIMEOUT_MS 5000U
#define AGENT_TRANSPORT_HANDSHAKE_TIMEOUT_MS 1000U

typedef enum
{
	AGENT_TRANSPORT_EVENT_DISCONNECTED,
	AGENT_TRANSPORT_EVENT_HEARTBEAT_TIMEOUT,
} AgentTransportEvent;

typedef void (*AgentTransportEventCallback)(void* context, AgentTransportEvent event,
	                                         uint64_t epoch);
typedef uint64_t (*AgentTransportClock)(void* context);
typedef void (*AgentTransportSendStartedCallback)(void* context, int fd, uint64_t epoch);

typedef struct
{
	AgentTransportEventCallback event;
	AgentTransportClock clock;
	void* context;
	/* Optional observability hook called with the captured fd before sending. */
	AgentTransportSendStartedCallback send_started;
} AgentTransportCallbacks;

typedef struct
{
	uint32_t sent_packets;
	uint32_t dropped_packets;
	uint32_t capture_frames;
	uint32_t dropped_frames;
} AgentTransportStats;

typedef struct
{
	pthread_mutex_t lock;
	pthread_mutex_t send_lock;
	AgentTransportCallbacks callbacks;
	int fd;
	int pending_fd;
	uint64_t epoch;
	uint64_t last_activity_at;
	uint64_t last_ping_at;
	bool stream_requested;
	bool supports_key_ack;
	bool heartbeat_timed_out;
	bool stopping;
	uint32_t waiting_key_sequence;
	bool key_ack_received;
	bool key_ack_success;
	AgentTransportStats stats;
} AgentTransport;

bool agent_transport_init(AgentTransport* transport, const AgentTransportCallbacks* callbacks);
void agent_transport_destroy(AgentTransport* transport);

/* Takes ownership of fd. Reads and validates HELLO before publishing a new epoch. */
bool agent_transport_accept(AgentTransport* transport, int fd, uint64_t* epoch);
/* Processes the accepted connection until EOF/error/replacement/shutdown. */
void agent_transport_run(AgentTransport* transport, int fd, uint64_t epoch);
void agent_transport_shutdown(AgentTransport* transport);

bool agent_transport_send(AgentTransport* transport, uint8_t type, const void* payload,
	                      uint16_t length);
bool agent_transport_set_stream_requested(AgentTransport* transport, bool requested);
void agent_transport_heartbeat(AgentTransport* transport, uint64_t now);
void agent_transport_get_stats(AgentTransport* transport, AgentTransportStats* stats);
bool agent_transport_is_current_epoch(AgentTransport* transport, uint64_t epoch);

bool agent_transport_supports_key_ack(AgentTransport* transport);
bool agent_transport_begin_key_ack(AgentTransport* transport, uint32_t sequence);
bool agent_transport_get_key_ack(AgentTransport* transport, uint32_t sequence,
	                             bool* received, bool* success);

#endif
