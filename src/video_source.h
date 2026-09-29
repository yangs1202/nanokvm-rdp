#ifndef NANOKVM_RDP_VIDEO_SOURCE_H
#define NANOKVM_RDP_VIDEO_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct VideoSource VideoSource;

/* The test seam uses the same ownership contract as RtpClient: read transfers
 * an allocated access unit to the caller, which must release it below. */
typedef struct
{
	bool (*open)(void* context, uint16_t port, int* receive_buffer_bytes);
	bool (*read)(void* context, uint8_t** data, size_t* length, uint32_t* timestamp,
	             uint32_t* ssrc, uint32_t* losses);
	void (*close)(void* context);
} VideoSourceRtpOps;

/* epoch is owned by VideoSource lifecycle only; it is independent from any
 * control/AgentTransport connection epoch. */
typedef bool (*VideoSourceIdrRequest)(void* context, uint64_t epoch);

typedef struct
{
	/* A successful open starts a source epoch and invokes request_idr. The
	 * producer must answer with SPS, PPS, then IDR for a fresh decoder;
	 * dependent access units are suppressed until all three are observed. */
	VideoSourceIdrRequest request_idr;
	void* request_idr_context;
	/* NULL selects the production RtpClient implementation. */
	const VideoSourceRtpOps* rtp_ops;
	void* rtp_context;
} VideoSourceConfig;

typedef struct
{
	uint8_t* data;
	size_t length;
	uint32_t timestamp;
	uint32_t ssrc;
	uint32_t losses;
	uint64_t epoch;
	bool packet_loss;
	bool idr_requested;
} VideoSourceAccessUnit;

VideoSource* video_source_create(const VideoSourceConfig* config);
void video_source_destroy(VideoSource* source);

bool video_source_open(VideoSource* source, uint16_t port);
bool video_source_read(VideoSource* source, VideoSourceAccessUnit* access_unit);
void video_source_close(VideoSource* source);

bool video_source_request_idr(VideoSource* source);
uint64_t video_source_epoch(const VideoSource* source);
int video_source_receive_buffer_bytes(const VideoSource* source);
void video_source_release_access_unit(VideoSourceAccessUnit* access_unit);

#endif
