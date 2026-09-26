#ifndef NANOKVM_FRAME_TRACE_H
#define NANOKVM_FRAME_TRACE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Diagnostic only: overwrite old records rather than ever blocking video. */
#define FRAME_TRACE_CAPACITY 256U

typedef struct
{
	bool valid;
	uint32_t frame_id;
	uint32_t ssrc;
	uint32_t rtp_timestamp;
	uint64_t received_at;
	uint64_t sent_at;
} FrameTraceEntry;

typedef struct
{
	FrameTraceEntry entries[FRAME_TRACE_CAPACITY];
} FrameTrace;

/* The current agent advances its identifier by 9000 per capture. This samples
 * about once per 50 captures, including across the uint32 timestamp wrap. */
static inline bool frame_trace_sample(uint32_t timestamp)
{
	return (timestamp / 9000U) % 50U == 0;
}

static inline uint64_t frame_trace_wall_ms(void)
{
	struct timespec now = { 0 };
	if (clock_gettime(CLOCK_REALTIME, &now) != 0) return 0;
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Caller serializes access; register before sending because ACKs can race. */
static inline void frame_trace_sent(FrameTrace* trace, uint32_t id, uint32_t ssrc,
                                    uint32_t timestamp, uint64_t received, uint64_t sent)
{
	trace->entries[id % FRAME_TRACE_CAPACITY] = (FrameTraceEntry){
		.valid = true, .frame_id = id, .ssrc = ssrc, .rtp_timestamp = timestamp,
		.received_at = received, .sent_at = sent
	};
}

static inline bool frame_trace_ack(FrameTrace* trace, uint32_t id, uint64_t now,
                                   FrameTraceEntry* entry)
{
	FrameTraceEntry* candidate = &trace->entries[id % FRAME_TRACE_CAPACITY];
	if (!candidate->valid || candidate->frame_id != id || now < candidate->sent_at)
		return false;
	*entry = *candidate;
	candidate->valid = false;
	return true;
}
#endif
