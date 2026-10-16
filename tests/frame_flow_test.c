#include "frame_flow.h"
#include <assert.h>
#include <stdio.h>

typedef struct
{
	uint32_t id;
	uint64_t at;
} SimulatedAck;

static void simulate_avc_stream(unsigned ack_delay_ms)
{
	FrameFlow flow = { 0 };
	SimulatedAck acks[FRAME_FLOW_LIMIT] = { 0 };
	unsigned ack_count = 0;
	uint64_t elapsed = 0;
	uint64_t now = 0;
	uint64_t wait_started_at = 0;
	uint32_t next_id = 1;
	unsigned sent = 0;
	unsigned resyncs = 0;

	for (unsigned frame = 0; frame < 300; frame++)
	{
		const uint64_t arrival = (uint64_t)frame * 33U;
		if (now < arrival)
			now = arrival;
		const bool idr = frame % 30U == 0;
		const uint32_t bytes = idr ? 300000U : 20000U;
		FrameFlowAvcAdmission admission;
		for (;;)
		{
			admission = frame_flow_avc_admit(&flow, bytes, idr, now, wait_started_at);
			if (admission != FRAME_FLOW_AVC_WAIT)
				break;
			if (wait_started_at == 0)
				wait_started_at = now;
			assert(ack_count > 0);
			if (now < acks[0].at)
				now = acks[0].at;
			for (unsigned i = 0; i < ack_count;)
			{
				if (acks[i].at > now)
				{
					i++;
					continue;
				}
				assert(frame_flow_ack(&flow, acks[i].id, 0, now, &elapsed));
				for (unsigned j = i; j + 1 < ack_count; j++)
					acks[j] = acks[j + 1];
				ack_count--;
			}
		}
		if (admission == FRAME_FLOW_AVC_RESYNC)
			resyncs++;
		assert(admission == FRAME_FLOW_AVC_SEND);
		const uint32_t id = next_id++;
		assert(idr ? frame_flow_sent_keyframe_sized(&flow, id, bytes, now)
		           : frame_flow_sent_sized(&flow, id, bytes, now));
		assert(ack_count < FRAME_FLOW_LIMIT);
		acks[ack_count++] = (SimulatedAck){ .id = id, .at = now + ack_delay_ms };
		wait_started_at = 0;
		sent++;
	}
	assert(sent == 300 && resyncs == 0);
}

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
	assert(frame_flow_interval(&flow) == 20 && !frame_flow_blocked(&flow));
	assert(!frame_flow_ack(&flow, 0, 0, 18001, &elapsed));
	assert(frame_flow_interval(&flow) == 40); /* resumption needs fresh measured ACKs */
	/* Production regression: backlog raised pacing to 62 ms, then the client
	 * suspended ACKs. The old code stayed at 62 ms indefinitely (~16 fps). */
	flow.interval_ms = 62;
	assert(!frame_flow_ack(&flow, 10, UINT32_MAX, 20000, &elapsed));
	assert(frame_flow_interval(&flow) == 20);
	for (unsigned i = 0; i < 100; i++)
	{
		assert(frame_flow_sent(&flow, i, 20001 + i * 20));
		frame_flow_send_cost(&flow, 9, 20010 + i * 20);
	}
	assert(frame_flow_interval(&flow) == 20 && flow.count == 0);
	/* A real slow send still backs off, then recovers without future ACKs. */
	frame_flow_send_cost(&flow, 80, 23000);
	assert(frame_flow_interval(&flow) == 28);
	for (unsigned i = 0; i < 32; i++) frame_flow_send_cost(&flow, 9, 23100 + i * 30);
	assert(frame_flow_interval(&flow) == 20);
	for (unsigned i = 0; i < 20; i++) frame_flow_send_cost(&flow, 80, 25000 + i * 100);
	assert(frame_flow_interval(&flow) > 20);
	assert(!frame_flow_ack(&flow, 100, 1000000, 28000, &elapsed));
	assert(!flow.suspended && frame_flow_interval(&flow) >= 40);
	/* A mobile client that ACKs for seconds, then stops, must not accumulate IDRs. */
	flow = (FrameFlow){ 0 };
	assert(frame_flow_sent_sized(&flow, 1, 400000, 1000));
	assert(!frame_flow_ready(&flow, 14000, 1100));
	assert(frame_flow_ack(&flow, 1, 0, 1200, &elapsed) && elapsed == 200);
	assert(flow.inflight_bytes == 0 && flow.last_ack_at == 1200);
	assert(frame_flow_sent_sized(&flow, 2, 14000, 1210));
	assert(!frame_flow_ready(&flow, 400000, 1300));
	assert(!frame_flow_ready(&flow, 14000, 1500));
	assert(frame_flow_ack(&flow, 2, 0, 1600, &elapsed));
	assert(frame_flow_ready(&flow, 400000, 1610));
	assert(frame_flow_sent_sized(&flow, 3, 400000, 1620));
	assert(!frame_flow_ready(&flow, 14000, 1640));
	/* Losing one P-frame breaks the whole AVC reference chain. Request one IDR,
	 * suppress all later P-frames, and keep waiting until an IDR is submitted. */
	flow = (FrameFlow){ 0 };
	bool need_idr = false;
	unsigned idr_requests = 0;
	for (uint32_t id = 0; id < FRAME_FLOW_LIMIT; id++)
		assert(frame_flow_sent(&flow, id, 3000 + id));
	assert(!frame_flow_ready(&flow, 100, 3100));
	if (frame_flow_avc_flow_rejected(&need_idr)) idr_requests++;
	assert(need_idr && idr_requests == 1);
	if (frame_flow_avc_flow_rejected(&need_idr)) idr_requests++;
	assert(idr_requests == 1);
	uint32_t ignored_bytes = 0;
	for (uint32_t id = 0; id < FRAME_FLOW_LIMIT; id++)
		assert(frame_flow_ack(&flow, id, 0, 3200 + id, &elapsed));
	assert(frame_flow_ready(&flow, 100, 3300));
	if (frame_flow_avc_should_send(need_idr, false, true)) ignored_bytes++;
	assert(ignored_bytes == 0);
	assert(frame_flow_avc_should_send(need_idr, true, false));
	/* A received IDR rejected by count or stale ACK is not a submitted IDR. */
	assert(frame_flow_sent(&flow, 10, 3400));
	flow.count = FRAME_FLOW_LIMIT;
	assert(!frame_flow_ready_keyframe(&flow, 3410));
	assert(need_idr);
	flow.count = 1;
	flow.last_ack_at = 3500;
	assert(!frame_flow_ready_keyframe(&flow, 3750));
	assert(need_idr);
	/* Unlike main's immediate byte-cap rejection, the first P-frame after a
	 * 300 KB IDR waits for its ACK and is then admitted. */
	flow = (FrameFlow){ 0 };
	assert(frame_flow_sent_keyframe_sized(&flow, 20, 300000, 1000));
	assert(!frame_flow_ready(&flow, 20000, 1010));
	assert(frame_flow_avc_admit(&flow, 20000, false, 1010, 0) == FRAME_FLOW_AVC_WAIT);
	assert(frame_flow_ack(&flow, 20, 0, 1020, &elapsed));
	assert(frame_flow_avc_admit(&flow, 20000, false, 1020, 1010) == FRAME_FLOW_AVC_SEND);
	simulate_avc_stream(15);
	simulate_avc_stream(60);
	/* A stalled client drops the expired frame and requests exactly one resync.
	 * P-frames remain suppressed until an IDR is actually submitted. */
	flow = (FrameFlow){ 0 };
	for (uint32_t id = 1; id <= FRAME_FLOW_LIMIT; id++)
		assert(frame_flow_sent(&flow, id, 4000 + id));
	bool stalled_need_idr = false;
	unsigned stalled_requests = 0;
	assert(frame_flow_avc_admit(&flow, 20000, false, 4100, 0) == FRAME_FLOW_AVC_WAIT);
	assert(frame_flow_avc_admit(&flow, 20000, false, 4250, 4100) == FRAME_FLOW_AVC_RESYNC);
	if (frame_flow_avc_flow_rejected(&stalled_need_idr)) stalled_requests++;
	assert(stalled_need_idr && stalled_requests == 1);
	assert(!frame_flow_avc_should_send(stalled_need_idr, false, true));
	if (frame_flow_avc_flow_rejected(&stalled_need_idr)) stalled_requests++;
	assert(stalled_requests == 1);
	for (uint32_t id = 1; id <= FRAME_FLOW_LIMIT; id++)
		assert(frame_flow_ack(&flow, id, 0, 4300 + id, &elapsed));
	assert(frame_flow_avc_admit(&flow, 300000, true, 4400, 0) == FRAME_FLOW_AVC_SEND);
	assert(frame_flow_sent_keyframe_sized(&flow, 4, 300000, 4400));
	frame_flow_avc_sent(&stalled_need_idr, true);
	assert(!stalled_need_idr && frame_flow_avc_should_send(stalled_need_idr, false, true));
	/* Suspended ACK flow admits oversized IDRs and P-frames immediately. */
	flow = (FrameFlow){ 0 };
	assert(!frame_flow_ack(&flow, 0, UINT32_MAX, 5000, &elapsed));
	assert(frame_flow_avc_admit(&flow, 300000, true, 5001, 0) == FRAME_FLOW_AVC_SEND);
	assert(frame_flow_avc_admit(&flow, 20000, false, 5002, 0) == FRAME_FLOW_AVC_SEND);
	/* IDR admission ignores the byte cap, while ordinary admission does not. */
	flow.suspended = false;
	flow.count = 1;
	flow.last_ack_at = 3800;
	flow.inflight_bytes = FRAME_FLOW_MAX_INFLIGHT_BYTES;
	assert(!frame_flow_ready(&flow, 400000, 3810));
	assert(frame_flow_ready_keyframe(&flow, 3810));
	assert(frame_flow_sent_keyframe_sized(&flow, 11, 400000, 3810));
	assert(flow.count == 2 && flow.inflight_bytes == FRAME_FLOW_MAX_INFLIGHT_BYTES + 400000U);
	assert(!frame_flow_ready(&flow, 1, 3820));
	frame_flow_avc_sent(&need_idr, true);
	assert(!need_idr);
	need_idr = false;
	frame_flow_avc_send_failed(&need_idr);
	assert(need_idr);
	flow.suspended = false;
	flow.count = 1;
	flow.last_ack_at = 1;
	flow.inflight_bytes = FRAME_FLOW_MAX_INFLIGHT_BYTES;
	assert(!frame_flow_ack(&flow, 0, UINT32_MAX, 1000, &elapsed));
	assert(frame_flow_ready_keyframe(&flow, 1000));
	assert(frame_flow_ready(&flow, 400000, 1000));
	puts("Frame flow: congestion, AVC reference recovery, suspend/resume and ACK tracking passed");
	return 0;
}
