# Frame latency tracing

AVC420 passthrough diagnostics correlate `FRAME_CAPTURE`, `FRAME_SEND`, and
`FRAME_ACK` by `(ssrc, rtp_ts)` within one agent run. The RTP timestamp is a
synthetic identifier, not a 90 kHz wall clock. Logs sample about one in 50
captures; the gateway keeps a bounded 256-entry ACK history and overwrites old
entries without delaying video. The existing codec and pacing policy is unchanged.

- `capture_call_ms`: time in `read_image`, including waiting for a frame. This
  does not measure HDMI capture/encoder pipeline age before the API returns.
- `packetize_send_ms`: capture return through UDP submission.
- `receive_to_send_ms`: complete RTP access unit receipt through RDP submission start.
- `submit_ms`: duration of the RDP SurfaceFrameCommand call, not wire delivery.
- `send_to_ack_ms`: RDP submission start through the matching client ACK.
  ACK is not physical display presentation. Missing/unmatched ACKs are unknown,
  not zero latency. queue_bytes=0 means unavailable and 4294967295 suspends ACKs.

Monotonic timestamps are comparable only on the same host. For cross-host
`receive_wall_ms - capture_wall_ms`, first measure both wall-clock offsets using
`tools/clock_probe.c` over persistent SSH/kubectl stdin/stdout connections.
Discard the startup sample. Each probe's offset lies between remote wall time
minus local reply time and remote wall time minus local request time. Use the
smallest round trip and report the combined uncertainty plus millisecond log
rounding; repeat calibration around the collection window to detect clock steps.
Neither these logs nor ACKs alone establish source-to-display latency.

The RISC-V build defaults to Release (`BUILD_TYPE=Debug` overrides it). The
agent retries transient UDP send errors with the same packet and a 40 ms
frame-wide retry deadline, sleeping 250 microseconds between attempts. Persistent
failure abandons the access unit and waits for the next IDR. `send_retries` is a
cumulative counter in sampled capture logs; `dropped` counts final failed packets,
not successful retries. The deadline bounds retries, not kernel delivery time.
