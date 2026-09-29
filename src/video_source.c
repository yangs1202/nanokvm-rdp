#include "video_source.h"

#include "h264.h"
#include "rtp_client.h"

#include <stdlib.h>
#include <string.h>

struct VideoSource
{
	RtpClient rtp;
	const VideoSourceRtpOps* rtp_ops;
	void* rtp_context;
	VideoSourceIdrRequest request_idr;
	void* request_idr_context;
	uint64_t epoch;
	uint32_t observed_losses;
	int receive_buffer_bytes;
	bool awaiting_idr;
	bool have_sps;
	bool have_pps;
	bool pending_packet_loss;
	bool pending_idr_requested;
	bool opened;
};

static bool production_open(void* context, uint16_t port, int* receive_buffer_bytes)
{
	RtpClient* rtp = context;
	if (!rtp_client_open(rtp, port))
		return false;
	*receive_buffer_bytes = rtp->receive_buffer_bytes;
	return true;
}

static bool production_read(void* context, uint8_t** data, size_t* length, uint32_t* timestamp,
	                         uint32_t* ssrc, uint32_t* losses)
{
	RtpClient* rtp = context;
	if (!rtp_client_read_h264(rtp, data, length))
		return false;
	*timestamp = rtp->access_unit_timestamp;
	*ssrc = rtp->access_unit_ssrc;
	*losses = rtp->losses;
	return true;
}

static void production_close(void* context)
{
	rtp_client_close(context);
}

static const VideoSourceRtpOps production_rtp_ops = {
	.open = production_open,
	.read = production_read,
	.close = production_close,
};

VideoSource* video_source_create(const VideoSourceConfig* config)
{
	VideoSource* source = calloc(1, sizeof(*source));
	if (!source)
		return NULL;
	source->rtp.fd = -1;
	if (config)
	{
		source->request_idr = config->request_idr;
		source->request_idr_context = config->request_idr_context;
		source->rtp_ops = config->rtp_ops;
		source->rtp_context = config->rtp_context;
	}
	if (!source->rtp_ops)
	{
		source->rtp_ops = &production_rtp_ops;
		source->rtp_context = &source->rtp;
	}
	if (!source->rtp_ops->open || !source->rtp_ops->read || !source->rtp_ops->close)
	{
		free(source);
		return NULL;
	}
	return source;
}

void video_source_destroy(VideoSource* source)
{
	if (!source)
		return;
	video_source_close(source);
	free(source);
}

bool video_source_open(VideoSource* source, uint16_t port)
{
	if (!source || source->opened || !source->rtp_ops->open(source->rtp_context, port,
	                                                        &source->receive_buffer_bytes))
		return false;
	source->opened = true;
	source->observed_losses = 0;
	source->awaiting_idr = true;
	source->have_sps = false;
	source->have_pps = false;
	source->pending_packet_loss = false;
	source->pending_idr_requested = false;
	source->epoch++;
	if (source->epoch == 0)
		source->epoch = 1;
	(void)video_source_request_idr(source);
	return true;
}

bool video_source_read(VideoSource* source, VideoSourceAccessUnit* access_unit)
{
	if (!source || !source->opened || !access_unit)
		return false;
	for (;;)
	{
		*access_unit = (VideoSourceAccessUnit){ 0 };
		if (!source->rtp_ops->read(source->rtp_context, &access_unit->data,
		                           &access_unit->length, &access_unit->timestamp,
		                           &access_unit->ssrc, &access_unit->losses))
			return false;
		access_unit->epoch = source->epoch;
		if (access_unit->losses != source->observed_losses)
		{
			source->pending_packet_loss = true;
			source->awaiting_idr = true;
			source->pending_idr_requested =
			    video_source_request_idr(source) || source->pending_idr_requested;
		}
		source->observed_losses = access_unit->losses;
		const bool has_sps = h264_contains_nal_type(access_unit->data, access_unit->length, 7);
		const bool has_pps = h264_contains_nal_type(access_unit->data, access_unit->length, 8);
		const bool has_idr = h264_contains_nal_type(access_unit->data, access_unit->length, 5);
		source->have_sps = source->have_sps || has_sps;
		source->have_pps = source->have_pps || has_pps;
		if (has_idr && source->have_sps && source->have_pps)
			source->awaiting_idr = false;
		if (!source->awaiting_idr || has_sps || has_pps)
		{
			access_unit->packet_loss = source->pending_packet_loss;
			access_unit->idr_requested = source->pending_idr_requested;
			source->pending_packet_loss = false;
			source->pending_idr_requested = false;
			return true;
		}
		video_source_release_access_unit(access_unit);
	}
}

void video_source_close(VideoSource* source)
{
	if (!source || !source->opened)
		return;
	source->rtp_ops->close(source->rtp_context);
	source->opened = false;
	source->observed_losses = 0;
	source->awaiting_idr = false;
	source->have_sps = false;
	source->have_pps = false;
	source->pending_packet_loss = false;
	source->pending_idr_requested = false;
	source->receive_buffer_bytes = 0;
}

bool video_source_request_idr(VideoSource* source)
{
	return source && source->opened && source->request_idr &&
	       source->request_idr(source->request_idr_context, source->epoch);
}

uint64_t video_source_epoch(const VideoSource* source)
{
	return source ? source->epoch : 0;
}

int video_source_receive_buffer_bytes(const VideoSource* source)
{
	return source ? source->receive_buffer_bytes : 0;
}

void video_source_release_access_unit(VideoSourceAccessUnit* access_unit)
{
	if (!access_unit)
		return;
	free(access_unit->data);
	*access_unit = (VideoSourceAccessUnit){ 0 };
}
