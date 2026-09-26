#include "frame_trace.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
	FrameTrace trace = { 0 };
	FrameTraceEntry entry;
	frame_trace_sent(&trace, 0, 42, 9001, 100, 105);
	frame_trace_sent(&trace, 1, 42, 18001, 110, 112);
	assert(frame_trace_ack(&trace, 1, 125, &entry));
	assert(entry.received_at == 110 && entry.sent_at == 112 && entry.rtp_timestamp == 18001);
	assert(!frame_trace_ack(&trace, 1, 126, &entry));
	assert(frame_trace_ack(&trace, 0, 130, &entry) && entry.ssrc == 42);
	/* An unresponsive or ACK-suspended client consumes bounded memory only. */
	for (uint32_t id = 0; id < 1000; id++) frame_trace_sent(&trace, id, 43, id, id, id + 1);
	assert(!frame_trace_ack(&trace, 0, 2000, &entry));
	assert(frame_trace_ack(&trace, 999, 2000, &entry));
	frame_trace_sent(&trace, UINT32_MAX, 44, 1, UINT64_C(5000000000), UINT64_C(5000000001));
	frame_trace_sent(&trace, 0, 44, 9001, UINT64_C(5000000010), UINT64_C(5000000011));
	assert(frame_trace_ack(&trace, UINT32_MAX, UINT64_C(5000000020), &entry));
	assert(frame_trace_ack(&trace, 0, UINT64_C(5000000021), &entry));
	assert(frame_trace_sample(1) && !frame_trace_sample(9001) && frame_trace_sample(450001));
	puts("Frame trace: correlation, duplicate/out-of-order ACK, overwrite and wrap passed");
}
