# Go gateway implementation and verification

The gateway refactor keeps the NanoKVM agent and its binary control/RTP protocol unchanged. The gateway process is now Go orchestration around a narrow C ABI:

- Go owns TCP control framing, reconnect and heartbeat policy, RTP/H.264 access-unit assembly, FFmpeg process lifecycle, media cancellation, input translation, and stream/session state.
- The C shim owns the FreeRDP listener, TLS/RDP peer callbacks, RDPGFX capability negotiation, bitmap codecs, AVC420/Progressive PDUs, and the FreeRDP input callback boundary.
- No HTTP, WebSocket, or browser transport was added. The external channels remain RDP/TLS TCP 3389, agent control TCP 3390, and RTP/H.264 UDP 5004 by default.

The shim uses a bounded ordered H.264 access-unit queue of four entries. Overflow drops the queued pictures, emits an overflow event, and suppresses P-frames until the next IDR. Bitmap frames are latest-only. This preserves H.264 reference ordering while preventing an unbounded capture-to-client backlog.

For direct AVC420, Go caches SPS and PPS NAL units even when the agent sends them as separate RTP access units. Parameter-set-only access units are not emitted as RDP pictures; the cached sets are prepended to an IDR when needed. RTP loss and shim queue overflow force the same IDR recovery gate, and P-frames remain suppressed until recovery.

Shutdown closes the control connections, cancels and closes the RTP socket, closes FFmpeg stdin, disconnects the FreeRDP socket, and joins all worker threads before releasing the cgo handle or C shim. The UDP loop checks cancellation after every successful read, so continuous RTP cannot postpone shutdown. A handshaking FreeRDP peer is interrupted through its socket descriptor before the peer thread is joined.

## Verification completed

- Native shim syntax check with `-Wall -Wextra -Werror`.
- Go tests with `-race`, including RTP STAP-A/FU-A/padding cases, late HELLO after shutdown, per-write control deadlines, horizontal-wheel translation, continuous-RTP shutdown, and occupied RDP-port error propagation.
- Baseline CTest: 7/7 passed.
- CMake gateway build and CLI help check passed against the installed FreeRDP 3 package.
- Local mock-agent plus FreeRDP smoke coverage observed TLS session activation, control `START_STREAM`/`STOP_STREAM`/`RELEASE_ALL`, FFmpeg decode, Progressive frames/ACKs, and direct AVC420 frames with the existing thin-client capability selection. The smoke is transport and protocol evidence, not physical display evidence.
- Idle TCP and incomplete TLS-handshake shutdown both exit normally in under the smoke harness timeout after a forced cgo rebuild.

The shim source and header live beside the Go package in `cmd/nanokvm-rdp`, so ordinary Go builds track C changes through cgo. The CMake packaging target still uses `go build -a` to make a clean release-style binary from the configured FreeRDP installation.

## Physical E2E acceptance status

Physical end-to-end acceptance is intentionally not claimed by this implementation milestone. No Windows App presentation or target-OS application interaction was available here, and no physical latency samples were recorded. The strict acceptance run must timestamp, per frame and per input event:

`screen capture → actual client presentation` and `input occurrence → target OS application effect`

excluding network time, and report p50, p95, p99, max, missing frames/events, and failures. Averages alone are insufficient; the result must explicitly distinguish client-side presentation from server enqueue, RDP send, or decoder completion.
