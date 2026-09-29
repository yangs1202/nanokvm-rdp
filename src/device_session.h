#ifndef NANOKVM_RDP_DEVICE_SESSION_H
#define NANOKVM_RDP_DEVICE_SESSION_H

#include "video_source.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct DeviceSession DeviceSession;

typedef enum
{
	DEVICE_SESSION_CONSUMER_DIRECT_GFX = 1,
	DEVICE_SESSION_CONSUMER_BITMAP = 2,
} DeviceSessionConsumer;

typedef struct
{
	DeviceSession* session;
	uint64_t generation;
	DeviceSessionConsumer consumer;
} DeviceSessionLease;

typedef struct
{
	uint16_t video_port;
	VideoSourceConfig video_source;
} DeviceSessionConfig;

DeviceSession* device_session_create(const DeviceSessionConfig* config);

/* Concurrency contract: a lease owner may read from one thread, but must stop
 * and join that thread before releasing the lease. All lease/read threads must
 * likewise be joined before destroying the session. release/destroy are not
 * cancellation primitives and must not run concurrently with read. */
void device_session_destroy(DeviceSession* session);

bool device_session_acquire(DeviceSession* session, DeviceSessionConsumer consumer,
	                        DeviceSessionLease* lease);
void device_session_release(DeviceSessionLease* lease);
bool device_session_read(DeviceSessionLease* lease, VideoSourceAccessUnit* access_unit);
int device_session_receive_buffer_bytes(const DeviceSessionLease* lease);

#endif
