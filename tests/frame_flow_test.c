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
	/* Fast feedback gradually raises FPS, never above the 16 ms floor. */
	flow = (FrameFlow){ 0 };
	assert(frame_flow_interval(&flow) == 40);
	for (uint32_t id = 0; id < 200; id++)
	{
		assert(frame_flow_sent(&flow, id, 1000 + id * 50U));
		assert(frame_flow_ack(&flow, id, 0, 1020 + id * 50U, &elapsed));
	}
	assert(frame_flow_interval(&flow) == 16);
	/* Duplicate ACKs cannot accelerate pacing. */
	assert(!frame_flow_ack(&flow, 199, 0, 12000, &elapsed));
	assert(flow.fast_acks == 0);
	/* Bursts of backlog feedback slow down once per 100 ms, not once per ACK. */
	assert(!frame_flow_ack(&flow, 900, 1000000, 13000, &elapsed));
	assert(frame_flow_interval(&flow) == 24);
	assert(!frame_flow_ack(&flow, 901, 1000000, 13001, &elapsed));
	assert(frame_flow_interval(&flow) == 24);
	assert(frame_flow_sent(&flow, 902, 13100));
	assert(frame_flow_ack(&flow, 902, 0, 13300, &elapsed));
	assert(frame_flow_interval(&flow) == 32);
	/* CPU/transport-bound sends back off as well. */
	frame_flow_send_cost(&flow, 50, 13400);
	assert(frame_flow_interval(&flow) == 40);
	for (unsigned i = 0; i < 30; i++) frame_flow_send_cost(&flow, 200, 14000 + i * 100U);
	assert(frame_flow_interval(&flow) == 100);
	flow.interval_ms = 16;
	assert(!frame_flow_ack(&flow, 0, UINT32_MAX, 18000, &elapsed));
	assert(frame_flow_interval(&flow) == 40 && !frame_flow_blocked(&flow));
	assert(!frame_flow_ack(&flow, 0, 0, 18001, &elapsed));
	assert(frame_flow_interval(&flow) == 40); /* resumption needs fresh measured ACKs */
	puts("Frame flow: slow client, duplicate/out-of-order ACK, suspend/resume and wrap passed");
	return 0;
}
