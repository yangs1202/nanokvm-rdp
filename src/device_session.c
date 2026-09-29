#include "device_session.h"

#include <pthread.h>
#include <stdlib.h>

struct DeviceSession
{
	pthread_mutex_t lock;
	VideoSource* source;
	uint16_t video_port;
	uint64_t generation;
	bool leased;
};

static bool lease_is_active(const DeviceSessionLease* lease)
{
	if (!lease || !lease->session || lease->generation == 0)
		return false;
	DeviceSession* session = lease->session;
	pthread_mutex_lock(&session->lock);
	const bool active = session->leased && session->generation == lease->generation;
	pthread_mutex_unlock(&session->lock);
	return active;
}

DeviceSession* device_session_create(const DeviceSessionConfig* config)
{
	if (!config || config->video_port == 0)
		return NULL;
	DeviceSession* session = calloc(1, sizeof(*session));
	if (!session)
		return NULL;
	if (pthread_mutex_init(&session->lock, NULL) != 0)
	{
		free(session);
		return NULL;
	}
	session->source = video_source_create(&config->video_source);
	if (!session->source)
	{
		video_source_destroy(session->source);
		pthread_mutex_destroy(&session->lock);
		free(session);
		return NULL;
	}
	session->video_port = config->video_port;
	return session;
}

void device_session_destroy(DeviceSession* session)
{
	if (!session)
		return;
	video_source_destroy(session->source);
	pthread_mutex_destroy(&session->lock);
	free(session);
}

bool device_session_acquire(DeviceSession* session, DeviceSessionConsumer consumer,
	                        DeviceSessionLease* lease)
{
	if (!session || !lease ||
	    (consumer != DEVICE_SESSION_CONSUMER_DIRECT_GFX &&
	     consumer != DEVICE_SESSION_CONSUMER_BITMAP))
		return false;
	*lease = (DeviceSessionLease){ 0 };
	pthread_mutex_lock(&session->lock);
	if (session->leased)
	{
		pthread_mutex_unlock(&session->lock);
		return false;
	}
	if (!video_source_open(session->source, session->video_port))
	{
		pthread_mutex_unlock(&session->lock);
		return false;
	}
	session->leased = true;
	session->generation++;
	if (session->generation == 0)
		session->generation = 1;
	lease->session = session;
	lease->generation = session->generation;
	lease->consumer = consumer;
	pthread_mutex_unlock(&session->lock);
	return true;
}

void device_session_release(DeviceSessionLease* lease)
{
	if (!lease || !lease->session)
		return;
	DeviceSession* session = lease->session;
	pthread_mutex_lock(&session->lock);
	if (session->leased && session->generation == lease->generation)
	{
		session->leased = false;
		video_source_close(session->source);
	}
	pthread_mutex_unlock(&session->lock);
	*lease = (DeviceSessionLease){ 0 };
}

bool device_session_read(DeviceSessionLease* lease, VideoSourceAccessUnit* access_unit)
{
	return lease_is_active(lease) && video_source_read(lease->session->source, access_unit);
}

int device_session_receive_buffer_bytes(const DeviceSessionLease* lease)
{
	return lease_is_active(lease)
	           ? video_source_receive_buffer_bytes(lease->session->source)
	           : 0;
}
