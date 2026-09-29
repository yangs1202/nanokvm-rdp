#include "device_session.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
	unsigned opens;
	unsigned closes;
	unsigned reads;
	unsigned reads_this_open;
	unsigned stale_datagrams_discarded;
	unsigned partial_access_units_reset;
	bool stale_datagram_queued;
	bool partial_access_unit;
} FakeRtp;

typedef struct
{
	uint64_t transport_epoch;
	uint64_t source_epochs[4];
	unsigned requests;
} FakeIdr;

static bool fake_open(void* context, uint16_t port, int* receive_buffer_bytes)
{
	FakeRtp* fake = context;
	assert(port == 5004);
	fake->opens++;
	fake->reads_this_open = 0;
	*receive_buffer_bytes = 65536;
	return true;
}

static bool fake_read(void* context, uint8_t** data, size_t* length, uint32_t* timestamp,
	                  uint32_t* ssrc, uint32_t* losses)
{
	FakeRtp* fake = context;
	if (fake->reads_this_open != 0)
	{
		errno = EAGAIN;
		return false;
	}
	const uint8_t access_unit[] = {
		0, 0, 0, 1, 0x67, 0x64,
		0, 0, 0, 1, 0x68, 0xee,
		0, 0, 0, 1, 0x65, (uint8_t)(0xa0U + fake->opens),
	};
	*data = malloc(sizeof(access_unit));
	assert(*data);
	memcpy(*data, access_unit, sizeof(access_unit));
	*length = sizeof(access_unit);
	*timestamp = fake->opens * 3000U;
	*ssrc = fake->opens == 1 ? 101U : 202U;
	*losses = 0;
	fake->reads++;
	fake->reads_this_open++;
	return true;
}

static void fake_close(void* context)
{
	FakeRtp* fake = context;
	fake->closes++;
	if (fake->stale_datagram_queued)
		fake->stale_datagrams_discarded++;
	if (fake->partial_access_unit)
		fake->partial_access_units_reset++;
	fake->stale_datagram_queued = false;
	fake->partial_access_unit = false;
}

static bool request_idr(void* context, uint64_t source_epoch)
{
	FakeIdr* idr = context;
	assert(idr->requests < sizeof(idr->source_epochs) / sizeof(idr->source_epochs[0]));
	idr->source_epochs[idr->requests++] = source_epoch;
	return true;
}

int main(void)
{
	FakeRtp fake = { 0 };
	FakeIdr idr = { .transport_epoch = 77 };
	const VideoSourceRtpOps operations = {
		.open = fake_open,
		.read = fake_read,
		.close = fake_close,
	};
	DeviceSession* session = device_session_create(&(DeviceSessionConfig){
		.video_port = 5004,
		.video_source = {
			.request_idr = request_idr,
			.request_idr_context = &idr,
			.rtp_ops = &operations,
			.rtp_context = &fake,
		},
	});
	assert(session);
	assert(fake.opens == 0 && fake.closes == 0);

	DeviceSessionLease direct = { 0 };
	DeviceSessionLease competing = { 0 };
	assert(device_session_acquire(session, DEVICE_SESSION_CONSUMER_DIRECT_GFX, &direct));
	assert(direct.consumer == DEVICE_SESSION_CONSUMER_DIRECT_GFX);
	assert(fake.opens == 1 && idr.requests == 1 && idr.source_epochs[0] == 1);
	assert(idr.transport_epoch == 77);
	assert(device_session_receive_buffer_bytes(&direct) == 65536);
	assert(!device_session_acquire(session, DEVICE_SESSION_CONSUMER_BITMAP, &competing));

	VideoSourceAccessUnit access_unit = { 0 };
	assert(device_session_read(&direct, &access_unit));
	assert(access_unit.epoch == 1 && access_unit.ssrc == 101 && access_unit.timestamp == 3000);
	assert(access_unit.data[access_unit.length - 1] == 0xa1);
	video_source_release_access_unit(&access_unit);

	DeviceSessionLease stale = direct;
	fake.stale_datagram_queued = true;
	fake.partial_access_unit = true;
	device_session_release(&direct);
	assert(fake.closes == 1 && fake.stale_datagrams_discarded == 1);
	assert(fake.partial_access_units_reset == 1);
	assert(!device_session_read(&stale, &access_unit));

	DeviceSessionLease bitmap = { 0 };
	assert(device_session_acquire(session, DEVICE_SESSION_CONSUMER_BITMAP, &bitmap));
	assert(bitmap.consumer == DEVICE_SESSION_CONSUMER_BITMAP);
	assert(fake.opens == 2 && idr.requests == 2 && idr.source_epochs[1] == 2);
	assert(idr.transport_epoch == 77);
	assert(device_session_read(&bitmap, &access_unit));
	assert(access_unit.epoch == 2 && access_unit.ssrc == 202 && access_unit.timestamp == 6000);
	assert(access_unit.data[access_unit.length - 1] == 0xa2);
	video_source_release_access_unit(&access_unit);
	device_session_release(&bitmap);

	assert(fake.opens == 2 && fake.closes == 2 && fake.reads == 2);
	device_session_destroy(session);
	assert(fake.closes == 2);
	puts("DeviceSession: reacquire resets stale RTP state, advances source epoch, and requests IDR");
	return 0;
}
