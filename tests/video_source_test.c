#include "video_source.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
	const uint8_t* data;
	size_t length;
	uint32_t timestamp;
	uint32_t ssrc;
	uint32_t losses;
} FakeRtpPacket;

typedef struct
{
	const FakeRtpPacket* packets;
	size_t packet_count;
	size_t next_packet;
	uint16_t opened_port;
	unsigned opens;
	unsigned closes;
} FakeRtpSocket;

typedef struct
{
	unsigned requests;
	uint64_t last_epoch;
} IdrRequests;

static bool fake_open(void* context, uint16_t port, int* receive_buffer_bytes)
{
	FakeRtpSocket* socket = context;
	socket->opened_port = port;
	socket->opens++;
	*receive_buffer_bytes = 32768;
	return true;
}

static bool fake_read(void* context, uint8_t** data, size_t* length, uint32_t* timestamp,
	                      uint32_t* ssrc, uint32_t* losses)
{
	FakeRtpSocket* socket = context;
	if (socket->next_packet == socket->packet_count)
	{
		errno = EAGAIN;
		return false;
	}
	const FakeRtpPacket* packet = &socket->packets[socket->next_packet++];
	*data = malloc(packet->length);
	assert(*data);
	memcpy(*data, packet->data, packet->length);
	*length = packet->length;
	*timestamp = packet->timestamp;
	*ssrc = packet->ssrc;
	*losses = packet->losses;
	return true;
}

static void fake_close(void* context)
{
	((FakeRtpSocket*)context)->closes++;
}

static bool request_idr(void* context, uint64_t epoch)
{
	IdrRequests* requests = context;
	requests->requests++;
	requests->last_epoch = epoch;
	return true;
}

int main(void)
{
	const uint8_t sps[] = { 0, 0, 0, 1, 0x67, 0x64 };
	const uint8_t pps[] = { 0, 0, 0, 1, 0x68, 0xee };
	const uint8_t idr[] = { 0, 0, 0, 1, 0x65, 0x88 };
	const uint8_t p_frame[] = { 0, 0, 0, 1, 0x41, 0x99 };
	const uint8_t sync[] = {
		0, 0, 0, 1, 0x67, 0x64,
		0, 0, 0, 1, 0x68, 0xee,
		0, 0, 0, 1, 0x65, 0x88,
	};
	const FakeRtpPacket packets[] = {
		{ .data = p_frame, .length = sizeof(p_frame), .timestamp = 87000, .ssrc = 42, .losses = 0 },
		{ .data = sps, .length = sizeof(sps), .timestamp = 90000, .ssrc = 42, .losses = 0 },
		{ .data = p_frame, .length = sizeof(p_frame), .timestamp = 91000, .ssrc = 42, .losses = 0 },
		{ .data = pps, .length = sizeof(pps), .timestamp = 92000, .ssrc = 42, .losses = 0 },
		{ .data = idr, .length = sizeof(idr), .timestamp = 93000, .ssrc = 42, .losses = 0 },
		{ .data = p_frame, .length = sizeof(p_frame), .timestamp = 94000, .ssrc = 42, .losses = 2 },
		{ .data = sync, .length = sizeof(sync), .timestamp = 95000, .ssrc = 42, .losses = 2 },
		{ .data = p_frame, .length = sizeof(p_frame), .timestamp = 96000, .ssrc = 42, .losses = 2 },
	};
	FakeRtpSocket socket = { .packets = packets, .packet_count = sizeof(packets) / sizeof(packets[0]) };
	IdrRequests requests = { 0 };
	const VideoSourceRtpOps rtp_ops = {
		.open = fake_open,
		.read = fake_read,
		.close = fake_close,
	};
	const VideoSourceConfig config = {
		.request_idr = request_idr,
		.request_idr_context = &requests,
		.rtp_ops = &rtp_ops,
		.rtp_context = &socket,
	};
	VideoSource* source = video_source_create(&config);
	assert(source);
	assert(video_source_open(source, 5004));
	assert(socket.opens == 1 && socket.opened_port == 5004);
	assert(video_source_epoch(source) == 1 && video_source_receive_buffer_bytes(source) == 32768);
	assert(requests.requests == 1 && requests.last_epoch == 1);

	VideoSourceAccessUnit access_unit = { 0 };
	assert(video_source_read(source, &access_unit));
	assert(access_unit.epoch == 1 && access_unit.timestamp == 90000 && access_unit.ssrc == 42);
	assert(access_unit.length == sizeof(sps) && memcmp(access_unit.data, sps, sizeof(sps)) == 0);
	assert(socket.next_packet == 2 && !access_unit.packet_loss && !access_unit.idr_requested);
	video_source_release_access_unit(&access_unit);

	assert(video_source_read(source, &access_unit));
	assert(access_unit.timestamp == 92000 && access_unit.length == sizeof(pps));
	assert(memcmp(access_unit.data, pps, sizeof(pps)) == 0 && socket.next_packet == 4);
	assert(!access_unit.packet_loss && !access_unit.idr_requested);
	video_source_release_access_unit(&access_unit);

	assert(video_source_read(source, &access_unit));
	assert(access_unit.timestamp == 93000 && access_unit.length == sizeof(idr));
	assert(memcmp(access_unit.data, idr, sizeof(idr)) == 0);
	assert(!access_unit.packet_loss && !access_unit.idr_requested);
	video_source_release_access_unit(&access_unit);

	assert(video_source_read(source, &access_unit));
	assert(access_unit.timestamp == 95000 && access_unit.length == sizeof(sync));
	assert(memcmp(access_unit.data, sync, sizeof(sync)) == 0);
	assert(access_unit.packet_loss && access_unit.idr_requested && access_unit.losses == 2);
	assert(requests.requests == 2 && requests.last_epoch == 1);
	video_source_release_access_unit(&access_unit);

	assert(video_source_read(source, &access_unit));
	assert(access_unit.timestamp == 96000 && !access_unit.packet_loss && !access_unit.idr_requested);
	video_source_release_access_unit(&access_unit);
	assert(video_source_request_idr(source));
	assert(requests.requests == 3 && requests.last_epoch == 1);
	assert(!video_source_read(source, &access_unit) && errno == EAGAIN);

	video_source_close(source);
	assert(socket.closes == 1);
	socket.next_packet = 0;
	assert(video_source_open(source, 5004));
	assert(video_source_epoch(source) == 2);
	assert(requests.requests == 4 && requests.last_epoch == 2);
	assert(video_source_read(source, &access_unit));
	assert(!access_unit.packet_loss && access_unit.epoch == 2 && access_unit.timestamp == 90000);
	assert(socket.next_packet == 2);
	video_source_release_access_unit(&access_unit);
	video_source_destroy(source);
	assert(socket.closes == 2);
	puts("VideoSource: epochs request SPS/PPS/IDR and suppress dependent frames until resync");
	return 0;
}
