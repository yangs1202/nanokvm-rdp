#include "frame_flow.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
	FrameFlow flow = { 0 };
	/* A decoder paused for seconds cannot acquire a growing backlog. */
	unsigned submitted = 0;
	for (unsigned frame = 0; frame < 1000; frame++)
		if (frame_flow_sent(&flow, frame, frame * 20U)) submitted++;
	assert(submitted == 3 && frame_flow_blocked(&flow));
	uint64_t elapsed;
	assert(!frame_flow_ack(&flow, 999, 0, 20000, &elapsed));
	assert(frame_flow_blocked(&flow));
	assert(frame_flow_ack(&flow, 1, 1024, 20000, &elapsed) && elapsed == 19980);
	assert(!frame_flow_blocked(&flow));
	assert(frame_flow_sent(&flow, 1000, 20001));
	assert(!frame_flow_ack(&flow, 1, 0, 20002, &elapsed));
	assert(frame_flow_blocked(&flow));
	assert(frame_flow_ack(&flow, 0, 0, 20003, &elapsed));
	assert(frame_flow_ack(&flow, 2, 0, 20004, &elapsed));
	assert(frame_flow_ack(&flow, 1000, 0, 20005, &elapsed) && elapsed == 4);
	assert(flow.count == 0);
	/* Opt-out must clear existing frames and never wait for absent ACKs. */
	assert(frame_flow_sent(&flow, UINT32_MAX, UINT64_C(5000000000)));
	assert(!frame_flow_ack(&flow, UINT32_MAX, UINT32_MAX, UINT64_C(5000000010), &elapsed));
	for (unsigned i = 0; i < 1000; i++) assert(frame_flow_sent(&flow, i, 0));
	assert(flow.suspended && flow.count == 0 && !frame_flow_blocked(&flow));
	/* Resumption, ID wrap, and uptime beyond 32 bits remain valid. */
	assert(!frame_flow_ack(&flow, 999, 0, UINT64_C(5000000011), &elapsed));
	assert(!flow.suspended);
	assert(frame_flow_sent(&flow, UINT32_MAX, UINT64_C(5000000020)));
	assert(frame_flow_sent(&flow, 0, UINT64_C(5000000021)));
	assert(frame_flow_ack(&flow, 0, 0, UINT64_C(5000000025), &elapsed) && elapsed == 4);
	assert(flow.count == 1);
	assert(frame_flow_ack(&flow, UINT32_MAX, 0, UINT64_C(5000000026), &elapsed) && elapsed == 6);
	puts("Frame flow: slow client, duplicate/out-of-order ACK, suspend/resume and wrap passed");
	return 0;
}
