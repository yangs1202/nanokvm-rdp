#ifndef NANOKVM_FRAME_FLOW_H
#define NANOKVM_FRAME_FLOW_H

#include <stdbool.h>
#include <stdint.h>

#define FRAME_FLOW_LIMIT 3U

typedef struct
{
	bool suspended;
	unsigned count;
	uint32_t ids[FRAME_FLOW_LIMIT];
	uint64_t sent_at[FRAME_FLOW_LIMIT];
} FrameFlow;

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
		flow->count = 0;
		return false;
	}
	flow->suspended = false;
	for (unsigned i = 0; i < flow->count; i++)
	{
		if (flow->ids[i] != id) continue;
		*elapsed = now - flow->sent_at[i];
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
