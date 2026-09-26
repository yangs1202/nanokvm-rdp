#ifndef NANOKVM_FRAME_FLOW_H
#define NANOKVM_FRAME_FLOW_H

#include <stdbool.h>
#include <stdint.h>

#define FRAME_FLOW_LIMIT 3U
#define FRAME_FLOW_DEFAULT_INTERVAL 40U
#define FRAME_FLOW_MIN_INTERVAL 16U
#define FRAME_FLOW_SUSPENDED_INTERVAL 20U
#define FRAME_FLOW_MAX_INTERVAL 100U

typedef struct
{
	bool suspended;
	bool have_ack;
	unsigned interval_ms;
	unsigned fast_acks;
	unsigned fast_sends;
	uint64_t last_slow_at;
	unsigned count;
	uint32_t ids[FRAME_FLOW_LIMIT];
	uint64_t sent_at[FRAME_FLOW_LIMIT];
} FrameFlow;

/* Unknown clients start conservatively; explicit ACK suspension permits 50 fps. */
static inline unsigned frame_flow_interval(const FrameFlow* flow)
{
	unsigned interval = flow->interval_ms ? flow->interval_ms : FRAME_FLOW_DEFAULT_INTERVAL;
	if (!flow->have_ack && !flow->suspended && interval < FRAME_FLOW_DEFAULT_INTERVAL)
		interval = FRAME_FLOW_DEFAULT_INTERVAL;
	if (flow->suspended && interval < FRAME_FLOW_SUSPENDED_INTERVAL)
		interval = FRAME_FLOW_SUSPENDED_INTERVAL;
	return interval;
}

static inline void frame_flow_slow(FrameFlow* flow, uint64_t now)
{
	flow->fast_acks = 0;
	flow->fast_sends = 0;
	if (flow->last_slow_at && now - flow->last_slow_at < 100U) return;
	unsigned interval = frame_flow_interval(flow) + 8U;
	flow->interval_ms = interval > FRAME_FLOW_MAX_INTERVAL ? FRAME_FLOW_MAX_INTERVAL : interval;
	flow->last_slow_at = now;
}

/* Include conversion and encoding cost; don't schedule faster than local work. */
static inline void frame_flow_send_cost(FrameFlow* flow, uint64_t cost_ms, uint64_t now)
{
	const unsigned interval = frame_flow_interval(flow);
	if (cost_ms >= interval) frame_flow_slow(flow, now);
	else if (flow->suspended && cost_ms <= interval / 2U)
	{
		/* No more ACKs will arrive. Recover from transient local stalls using
		 * successful sends, instead of permanently retaining a slow interval. */
		if (++flow->fast_sends >= 8U)
		{
			flow->interval_ms = interval > FRAME_FLOW_SUSPENDED_INTERVAL + 1U
			                        ? interval - 2U : FRAME_FLOW_SUSPENDED_INTERVAL;
			flow->fast_sends = 0;
		}
	}
	else flow->fast_sends = 0;
}

static inline bool frame_flow_blocked(const FrameFlow* flow)
{
	return !flow->suspended && flow->count == FRAME_FLOW_LIMIT;
}

static inline bool frame_flow_sent(FrameFlow* flow, uint32_t id, uint64_t now)
{
	if (flow->suspended) return true;
	if (frame_flow_blocked(flow)) return false;
	flow->ids[flow->count] = id;
	flow->sent_at[flow->count++] = now;
	return true;
}

/* Each ACK names one frame; do not release others on duplicate/out-of-order ACKs. */
static inline bool frame_flow_ack(FrameFlow* flow, uint32_t id, uint32_t depth,
                                 uint64_t now, uint64_t* elapsed)
{
	*elapsed = 0;
	if (depth == UINT32_MAX)
	{
		flow->suspended = true;
		flow->have_ack = false;
		flow->fast_acks = 0;
		/* MS-RDPEGFX 3.2.5.13: assume the client decodes faster than delivery.
		 * Discard stale congestion history; transport backpressure still applies. */
		flow->interval_ms = FRAME_FLOW_SUSPENDED_INTERVAL;
		flow->fast_sends = 0;
		flow->last_slow_at = 0;
		flow->count = 0;
		return false;
	}
	flow->suspended = false;
	/* A queued byte count is useful even for ACKs sent before tracking resumed. */
	if (depth > 256U * 1024U) frame_flow_slow(flow, now);
	for (unsigned i = 0; i < flow->count; i++)
	{
		if (flow->ids[i] != id) continue;
		*elapsed = now - flow->sent_at[i];
		flow->have_ack = true;
		if (*elapsed > 120U) frame_flow_slow(flow, now);
		else if (depth == 0 && *elapsed <= 60U)
		{
			if (++flow->fast_acks >= 8U)
			{
				const unsigned interval = frame_flow_interval(flow);
				flow->interval_ms = interval > FRAME_FLOW_MIN_INTERVAL + 1U
				                        ? interval - 2U : FRAME_FLOW_MIN_INTERVAL;
				flow->fast_acks = 0;
			}
		}
		else flow->fast_acks = 0;
		flow->count--;
		for (unsigned j = i; j < flow->count; j++)
		{
			flow->ids[j] = flow->ids[j + 1];
			flow->sent_at[j] = flow->sent_at[j + 1];
		}
		return true;
	}
	return false;
}

#endif
