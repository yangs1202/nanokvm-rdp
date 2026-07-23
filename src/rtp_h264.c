#include "rtp_h264.h"

#include <stdlib.h>
#include <string.h>

#define RTP_HEADER_SIZE 12U
#define RTP_PAYLOAD_TYPE 96U

static void write_u16(uint8_t* data, uint16_t value)
{
	data[0] = (uint8_t)(value >> 8U);
	data[1] = (uint8_t)value;
}

static uint16_t read_u16(const uint8_t* data)
{
	return ((uint16_t)data[0] << 8U) | data[1];
}

static void write_u32(uint8_t* data, uint32_t value)
{
	data[0] = (uint8_t)(value >> 24U);
	data[1] = (uint8_t)(value >> 16U);
	data[2] = (uint8_t)(value >> 8U);
	data[3] = (uint8_t)value;
}

static uint32_t read_u32(const uint8_t* data)
{
	return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) |
	       ((uint32_t)data[2] << 8U) | data[3];
}

static bool emit_packet(RtpH264Packetizer* packetizer, const uint8_t* payload, size_t length,
	                    bool marker, uint32_t timestamp, RtpH264PacketCallback callback,
	                    void* context)
{
	uint8_t packet[RTP_H264_DEFAULT_MTU] = { 0 };
	if (length + RTP_HEADER_SIZE > sizeof(packet) || !callback)
		return false;
	packet[0] = 0x80;
	packet[1] = (uint8_t)(RTP_PAYLOAD_TYPE | (marker ? 0x80U : 0));
	write_u16(packet + 2, packetizer->sequence++);
	write_u32(packet + 4, timestamp);
	write_u32(packet + 8, packetizer->ssrc);
	memcpy(packet + RTP_HEADER_SIZE, payload, length);
	return callback(context, packet, RTP_HEADER_SIZE + length);
}

void rtp_h264_packetizer_init(RtpH264Packetizer* packetizer, uint16_t mtu, uint32_t ssrc)
{
	*packetizer = (RtpH264Packetizer){ .sequence = 1, .ssrc = ssrc,
		.mtu = mtu < RTP_HEADER_SIZE + 3 ? RTP_H264_DEFAULT_MTU : mtu };
	if (packetizer->mtu > RTP_H264_DEFAULT_MTU)
		packetizer->mtu = RTP_H264_DEFAULT_MTU;
}

bool rtp_h264_packetize_marker(RtpH264Packetizer* packetizer, const uint8_t* nal, size_t length,
	                            uint32_t timestamp, bool marker,
	                            RtpH264PacketCallback callback, void* context)
{
	if (!packetizer || !nal || length == 0 || length > RTP_H264_MAX_NAL)
		return false;
	const size_t payload_limit = packetizer->mtu - RTP_HEADER_SIZE;
	if (length <= payload_limit)
		return emit_packet(packetizer, nal, length, marker, timestamp, callback, context);
	const size_t fragment_limit = payload_limit - 2U;
	if (fragment_limit == 0)
		return false;
	for (size_t offset = 1; offset < length;)
	{
		const size_t fragment = (length - offset) < fragment_limit ? length - offset : fragment_limit;
		uint8_t payload[RTP_H264_DEFAULT_MTU] = { 0 };
		payload[0] = (uint8_t)((nal[0] & 0xe0U) | 28U);
		payload[1] = (uint8_t)((nal[0] & 0x1fU) | (offset == 1 ? 0x80U : 0) |
		                       (offset + fragment == length ? 0x40U : 0));
		memcpy(payload + 2, nal + offset, fragment);
		if (!emit_packet(packetizer, payload, fragment + 2,
		                 marker && offset + fragment == length, timestamp,
		                 callback, context))
			return false;
		offset += fragment;
	}
	return true;
}

bool rtp_h264_packetize(RtpH264Packetizer* packetizer, const uint8_t* nal, size_t length,
	                     uint32_t timestamp, RtpH264PacketCallback callback, void* context)
{
	return rtp_h264_packetize_marker(packetizer, nal, length, timestamp, true, callback, context);
}

void rtp_h264_reassembler_init(RtpH264Reassembler* reassembler)
{
	*reassembler = (RtpH264Reassembler){ 0 };
}

void rtp_h264_reassembler_free(RtpH264Reassembler* reassembler)
{
	free(reassembler->buffer);
	*reassembler = (RtpH264Reassembler){ 0 };
}

static bool append(RtpH264Reassembler* reassembler, const uint8_t* data, size_t length)
{
	if (length > RTP_H264_MAX_NAL - reassembler->length)
		return false;
	const size_t needed = reassembler->length + length;
	if (needed > reassembler->capacity)
	{
		size_t capacity = reassembler->capacity ? reassembler->capacity : 2048U;
		while (capacity < needed)
			capacity *= 2U;
		uint8_t* resized = realloc(reassembler->buffer, capacity);
		if (!resized)
			return false;
		reassembler->buffer = resized;
		reassembler->capacity = capacity;
	}
	memcpy(reassembler->buffer + reassembler->length, data, length);
	reassembler->length += length;
	return true;
}

static bool deliver(RtpH264Reassembler* reassembler, RtpH264NalCallback callback, void* context,
	               bool marker)
{
	const bool result = callback && callback(context, reassembler->buffer, reassembler->length,
	                                         reassembler->timestamp, marker);
	reassembler->assembling = false;
	reassembler->length = 0;
	return result;
}

bool rtp_h264_reassembler_push(RtpH264Reassembler* reassembler, const uint8_t* packet,
	                           size_t length, RtpH264NalCallback nal_callback, void* nal_context,
	                           RtpH264LossCallback loss_callback, void* loss_context)
{
	if (!reassembler || !packet || length <= RTP_HEADER_SIZE || (packet[0] >> 6U) != 2U)
		return false;
	const uint16_t sequence = read_u16(packet + 2);
	if (reassembler->have_sequence && sequence != reassembler->expected_sequence)
	{
		reassembler->assembling = false;
		reassembler->length = 0;
		if (loss_callback)
			loss_callback(loss_context);
	}
	reassembler->have_sequence = true;
	reassembler->expected_sequence = (uint16_t)(sequence + 1U);
	const uint32_t timestamp = read_u32(packet + 4);
	const bool marker = (packet[1] & 0x80U) != 0;
	const uint8_t* payload = packet + RTP_HEADER_SIZE;
	const size_t payload_length = length - RTP_HEADER_SIZE;
	const uint8_t type = payload[0] & 0x1fU;
	if (type >= 1U && type <= 23U)
		return nal_callback && nal_callback(nal_context, payload, payload_length, timestamp, marker);
	if (type != 28U || payload_length < 3)
		return true;
	const bool start = (payload[1] & 0x80U) != 0;
	const bool end = (payload[1] & 0x40U) != 0;
	if (start)
	{
		reassembler->assembling = true;
		reassembler->timestamp = timestamp;
		reassembler->length = 0;
		reassembler->nal_header = (uint8_t)((payload[0] & 0xe0U) | (payload[1] & 0x1fU));
		if (!append(reassembler, &reassembler->nal_header, 1))
			return false;
	}
	if (!reassembler->assembling || timestamp != reassembler->timestamp ||
	    !append(reassembler, payload + 2, payload_length - 2))
		return false;
	return end ? deliver(reassembler, nal_callback, nal_context, marker) : true;
}
