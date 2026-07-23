#ifndef NANOKVM_RDP_RTP_H264_H
#define NANOKVM_RDP_RTP_H264_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTP_H264_DEFAULT_MTU 1200U
#define RTP_H264_MAX_NAL (2U * 1024U * 1024U)

typedef bool (*RtpH264PacketCallback)(void* context, const uint8_t* packet, size_t length);
typedef bool (*RtpH264NalCallback)(void* context, const uint8_t* nal, size_t length,
	                              uint32_t timestamp);
typedef void (*RtpH264LossCallback)(void* context);

typedef struct
{
	uint16_t sequence;
	uint32_t ssrc;
	uint16_t mtu;
} RtpH264Packetizer;

typedef struct
{
	uint16_t expected_sequence;
	bool have_sequence;
	bool assembling;
	uint8_t* buffer;
	size_t length;
	size_t capacity;
	uint8_t nal_header;
	uint32_t timestamp;
} RtpH264Reassembler;

void rtp_h264_packetizer_init(RtpH264Packetizer* packetizer, uint16_t mtu, uint32_t ssrc);
bool rtp_h264_packetize(RtpH264Packetizer* packetizer, const uint8_t* nal, size_t length,
	                     uint32_t timestamp, RtpH264PacketCallback callback, void* context);
void rtp_h264_reassembler_init(RtpH264Reassembler* reassembler);
void rtp_h264_reassembler_free(RtpH264Reassembler* reassembler);
bool rtp_h264_reassembler_push(RtpH264Reassembler* reassembler, const uint8_t* packet,
	                           size_t length, RtpH264NalCallback nal_callback, void* nal_context,
	                           RtpH264LossCallback loss_callback, void* loss_context);

#endif
