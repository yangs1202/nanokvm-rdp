# Main-task review ledger

Implementation task: `01a0dd35-6ab1-74d2-a7ae-bc5d85ec94ce` (GPT-5.6 Luna).
Implementation worktree: `/Users/yangs/.codex/worktrees/af02/nanokvm-rdp`.
This is an early review ledger, not approval of an unfinished implementation.

## Baseline

- Baseline C unit/integration suite: 7/7 passed.
- Baseline gateway built against installed FreeRDP 3.32.0 with CMake.
- Linux verification environment: existing Colima VM started; use explicit `docker --context colima` (Linux aarch64 with amd64 emulation). Global Docker context restored to its original `default`. Emulated performance is not acceptance evidence. Production Dockerfile pins FreeRDP 3.14.0, so host compilation alone is insufficient.
- Final acceptance environment and latency boundaries: `go-gateway-acceptance.md`.

## Findings sent to implementation

1. **Compressed-frame dependencies:** latest-only replacement is appropriate for decoded BGRA, not arbitrary H.264 P-frames. Require ordered bounded handling and IDR recovery on overflow. Header contract revised; implementation/tests pending verification.
2. **Input failure propagation:** input callback must return transport failure. Header changed from void to bool; implementation/tests pending verification.
3. **Codec compatibility:** do not replace negotiated Progressive with classic bitmap. Preserve Progressive encoding and frame-ACK flow control used by the existing Windows App path.
4. **Dynamic channels:** preserve actual RDPGFX open/message handling and classic fallback on unsupported/timed-out negotiation.
5. **Lifetime:** detached peer threads must terminate before shim locks, callback handles and buffers are freed, including peers still handshaking. Synchronize stop flags; avoid callback reentrancy while holding locks.
6. **Event-handle bounds:** reserve space for both channel and frame event handles before appending them to the fixed-size array.
7. **Session ownership:** rejected or failed peers must not emit a stop event for another client's active stream.
8. **Input compatibility:** preserve Alt/Command mapping, extended pointer flag conversion, wheel separation, relative-pointer button state, Unicode conversion and synchronization behavior across the Go/C boundary.
9. **FreeRDP context registration:** independent `cc -fsyntax-only -Wall -Wextra -Werror` found unused context callbacks. Register `ContextSize`, `ContextNew` and `ContextFree` before context allocation; this is also necessary for the derived context's memory layout.
10. **Join completion:** a timed wait followed by unconditional handle close/free is not a join guarantee. Never free the shim while a peer remains live after the timeout.

All findings require verification against the final diff and runnable tests before closure.

## Go RTP reproduction results

Independent tests against the first Go RTP implementation snapshot, in `build/refactor-review/rtp_review_test.go`, failed in three cases:

- Two-NAL STAP-A packet: only SPS delivered, second PPS missing (length offset advanced incorrectly).
- Unfinished FU-A followed by a new timestamp: truncated IDR delivered as a complete unit.
- RTP padding flag: padding bytes incorrectly included in the H.264 access unit.

Sent the reproducible tests to Luna for correction and inclusion in the implementation's regression suite. Also flagged absent AU byte bounds and FFmpeg shutdown deadlock potential: `push` holds the mutex during a blocking pipe write while `close` requires the same mutex before closing the pipe.

First retest of the revised snapshot: STAP-A and incomplete FU-A reproductions pass; padding still fails (`0000000165aa0002` includes the two padding bytes). This closes neither the complete RTP review nor the performance gate.

## Go lifecycle review

The initial gateway orchestration also needs these fixes (sent to Luna):

- Refresh TCP write deadlines per operation: setting an absolute deadline only at accept makes every write after the first 100 ms expire.
- Propagate shim startup/termination errors into `Run`; handle `Close` before `Run` and listener startup failure without waiting on an unstarted worker.
- Join RTP and decoded-frame consumer goroutines before freeing the shim or starting the next session. Tag work with a session generation to prevent cross-session delivery.
- Close decoder frame delivery and cancel the process/read/write paths so shutdown cannot leak consumers or deadlock on pipe writes.
- Bind the video receiver and initialize the chosen pipeline before asking the agent to stream; do not discard the first IDR while initializing the decoder.
- Preserve RDP disconnection/stream shutdown on agent heartbeat expiry; prevent replaced or unfinished agent handshakes from acting on a newer connection or installing an agent after shutdown.

## Independent runtime retest

Copied a current implementation snapshot into ignored `build/refactor-review-core` to avoid editing Luna's files. Ran `go test -race -v -timeout 10s ./cmd/nanokvm-rdp -run TestReview`: six tests passed (close before run, startup error with occupied RDP port, refreshing an expired TCP write deadline, and the three RTP regression cases). This snapshot's padding regression is now fixed.

The run also logged WinPR `CreateEventA: auto-reset events not yet implemented` on macOS. Sent this platform issue to Luna for a manual-reset event implementation and real successful connection verification. The startup-error test only proves an error returns rather than hanging; it does not prove successful initialization or uniquely attribute the error to bind failure. Go's race detector also does not prove C code free of data races.

## Local RDP negotiation smoke

Built the new Go CLI and ran it on loopback ports 43989/43990/43991 with a disposable self-signed TLS certificate. `sdl-freerdp` with SDL dummy drivers connected using TLS and a 640×360 Progressive request. A local mock agent sent HELLO and answered heartbeats.

Observed START_STREAM, KEY (three-byte payload), SYNCHRONIZE, recurring PING and STOP_STREAM after a six-second session. Gateway terminated with exit 0. This proves local RDP activation and basic control dispatch; the mock sends no video and does not prove decoded-frame delivery, physical input application or latency. Latest CLI build did not produce the earlier auto-reset event error. Reproducible local harness: `build/refactor-review/rdp_smoke.py`.

Extended the harness with FFmpeg `testsrc2` at 640×360, 30 fps for four seconds, libx264 ultrafast/zerolatency, RTP payload 96, packet size 1200. The SDL client trace reported 88 CAPROGRESSIVE frames and frame 87 end/ACK. Agent observed STOP_STREAM and RELEASE_ALL after client termination; gateway exited 0. This exercises RTP→FFmpeg→Progressive→FreeRDP client, but dummy-driver ACK is not physical presentation and no E2E latency claim is made. A second agent connection appeared shortly before final shutdown; avoid overlapping harness ports in subsequent runs.

Two further smoke runs requested AVC420 and AVC444 respectively, but both negotiated Progressive (88 frames, zero AVC420). Direct H.264 passthrough therefore remains unverified; changing negotiation merely to make this test pass would not establish Windows App compatibility.

Latest lifecycle review still requires a shutdown barrier for the heartbeat, accept loop and pending HELLO handlers before freeing the shim. A heartbeat already executing can call into the freed shim; a delayed HELLO can install an agent after shutdown. Also move decoded-frame worker registration before releasing the synchronization that permits shutdown to wait. Invalid listen addresses must not silently become wildcard control/RTP binds through `net.ParseIP` returning nil. Findings sent to Luna for implementation and regression coverage.

`TestReviewLateHelloAfterShutdown` independently reproduced the delayed HELLO bug against the current Go gateway snapshot: start a `net.Pipe` handler, close the gateway, send HELLO then PING, receive PONG, and observe an installed agent. The regression failed as expected and was sent to Luna. Test lives in ignored `build/refactor-review-core/cmd/nanokvm-rdp/shutdown_review_test.go`.

## CMake Go executable verification

Configured the Luna source tree into the main tree's ignored `build/refactor-go-cmake` with installed FreeRDP, server enabled, agent disabled, and tests enabled. `cmake --build ... -j4` produced the Go gateway, its `-h` exited successfully, and `ctest --test-dir build/refactor-go-cmake --output-on-failure` passed all seven existing C tests. These tests cover retained C helpers; they do not replace Go regression tests, Linux packaging, or the physical latency acceptance gate.

## Input compatibility regression

Baseline `on_relative_mouse` maps Windows App's nonzero horizontal delta with zero vertical delta and vertical-wheel flag into horizontal wheel, preserving direction. The Go snapshot discarded those deltas. `TestReviewWindowsAppHorizontalWheel` failed both directions: expected flags `0478`/`0578`, received `0278` in both. Reproduction in ignored `build/refactor-review-core/cmd/nanokvm-rdp/input_review_test.go` sent to Luna. Extended pointer flags also need the baseline mask before forwarding.

The local video harness now accepts resolution and port environment variables. A 1920×1080, 30 fps, four-second Progressive run using the previously built snapshot on ports 44989–44991 delivered 88 client codec PDUs and shut down with gateway exit 0. This broadens the functional smoke to the default source resolution but does not establish startup latency, steady-state latency, or frame-drop statistics.

## Direct AVC420 local smoke

FreeRDP 3.32's [capability advertisement implementation](https://github.com/FreeRDP/FreeRDP/blob/3.32.0/channels/rdpgfx/client/rdpgfx_main.c#L394-L398) sets AVC_THINCLIENT when the thin-client setting is enabled with AVC support. Running the unchanged gateway snapshot with `SMOKE_GFX='AVC444:on,thin-client:on' SMOKE_WIDTH=1920 SMOKE_HEIGHT=1080 SMOKE_PORT=44989` selected direct AVC420. The four-second 30 fps source produced 120 AVC420 client codec PDUs, zero Progressive PDUs, STOP_STREAM/RELEASE_ALL, and gateway exit 0. This resolves the earlier local codec-selection gap; physical Windows App latency and visual correctness still require the specified environment.

After refreshing the isolated review snapshot's `gateway.go`, the independent late-HELLO and both horizontal-scroll direction regressions passed under `go test -race`. This verifies those concrete reproductions were corrected. The isolated snapshot still contains the older C shim and its auto-reset warning; this targeted test is not a final shim/lifecycle acceptance run.

## Active-stream shutdown reproduction

Current worker `go test -race -timeout 30s ./cmd/nanokvm-rdp` passed. Rebuilt the actual CLI and repeated 1080p Progressive smoke (104 codec PDUs, exit 0). Then tested SIGTERM while the client and RTP sender remained active. With a four-second source, termination at two seconds waited about 1969.6 ms. Extending the source to twenty seconds made shutdown exceed the three-second bound and require SIGKILL (exit -9).

Reproduction: `SMOKE_SERVER_FIRST=1 SMOKE_SECONDS=2 SMOKE_VIDEO_SECONDS=20 SMOKE_WIDTH=640 SMOKE_HEIGHT=360 SMOKE_PORT=44989 python3 build/refactor-review/rdp_smoke.py`. The Go receive loop checks cancellation only after a failed UDP read, so continuous traffic prevents termination. Reported to Luna to close/unblock the receiver on cancellation and cover continuous UDP in a regression. The local fixture intentionally keeps sending after control disconnect; shutdown must remain bounded in that condition.

`build/refactor-review/handshake_shutdown.py` covers incomplete negotiation without an agent. An idle TCP connection shut down in 102 ms with exit 0. A peer that negotiated TLS successfully and then withheld ClientHello prevented shutdown for over three seconds and required SIGKILL. Reported to Luna: stopping only the active session does not interrupt every handshaking peer; abort blocked transport work before joining/freeing it.

After the receiver-close fix, the same continuous twenty-second RTP fixture terminated normally in 138.3 ms (exit 0). After the peer-socket interruption fix and a forced `go build -a`, idle TCP and incomplete TLS terminated normally in 100.6/92.5 ms respectively (exit 0). These are shutdown measurements, not input/video latency claims.

The TLS retest exposed a build correctness issue: ordinary cached `go build` retained the old shim after edits to `src/rdp_shim.c`, which is textually included from a C file inside the Go package. A forced build picked up the fix and changed the result. Requested moving the real shim source/header inside the cgo package or another explicit dependency strategy so normal incremental builds/tests cannot silently use stale C code.

## Non-RDPGFX smoke

Extended the harness with `SMOKE_LEGACY` to omit server `-direct-gfx` and disable client GFX. A 640×360 four-second run reported 107 `RDP_STATS_SURFACE_BITS_RFX` updates, STOP_STREAM/RELEASE_ALL and gateway exit 0. Attempts to select NSCodec and classic bitmap with client flags also reported RemoteFX (106/107 updates); those two codecs are therefore not verified by these runs. This is functional RemoteFX evidence only, not pixel comparison or physical latency evidence.

## Agent-style separate H.264 parameter sets

Baseline direct-video handling caches SPS/PPS, emits only pictures, and prepends cached configuration to IDRs. The new direct path initially forwarded every access unit, including parameter-only units, and its overflow suppression also discarded parameter sets.

Added `SMOKE_SPLIT_PARAMS=1` to the local harness. Its RTP relay separates only SPS/PPS into individual marked timestamps, preserving the original grouping of picture slices. Four seconds generated 120 picture units plus four SPS and four PPS units. The client received 128 AVC420 PDUs and logged eight `Failed to decode video frame` errors. Gateway exit 0 does not make that run pass. Sent the fixture to Luna to restore parameter caching, picture filtering, IDR composition and recovery behavior. The initial relay draft split all slices and is not the fixture used for this recorded result.

## Linux build status

Luna's first linux/amd64 Docker build exited 2 during FreeRDP compilation; Luna reported a compiler segmentation fault under QEMU. Independent environment inspection confirms Colima uses VZ/aarch64, four CPUs, 8 GiB RAM, and qemu-x86_64 binfmt with Rosetta disabled. The failed build container did not report OOMKilled. Requested a native linux/arm64 build to validate Linux packaging and a lower-parallelism amd64 retry; neither the Linux build nor production-architecture compatibility is considered passed yet.

## RTP recovery follow-up

Added three independent cases to `build/refactor-review/rtp_review_test.go`; the original three still pass, these three fail against the current parser:

- A marker on an unfinished FU-A emits truncated data despite a missing FU end bit.
- A complete new IDR at a new timestamp is discarded because the preceding timestamp had an unfinished FU-A.
- A truncated second STAP-A NAL causes the valid prefix to be emitted as a complete access unit.

Sent the reproductions to Luna as part of the H.264 recovery review. Parser validation must reject incomplete units while allowing the next independently complete recovery frame.

After Luna's fixes, all six independent RTP cases passed against a refreshed source snapshot. A forced rebuilt gateway also passed the separate-SPS/PPS client smoke: 128 incoming units produced exactly 120 AVC420 picture PDUs with zero decode errors and exit 0. A follow-up relay run deliberately dropped one P-picture (ten RTP packets): the client received 108 picture PDUs, logged no decode errors, and the mock observed two IDR requests. The mock waits for its periodic IDR rather than responding immediately, so this establishes reference recovery, not a latency bound.

The native arm64 image subsequently compiled FreeRDP and failed at the Go application stage. Independent reproduction from that build's frozen input image identified missing `WINPR_C_ARRAY_INIT` compatibility (available on host FreeRDP 3.32, absent in 3.14) and then missing OpenSSL references because plain pkg-config omitted static private dependencies. With a temporary compatibility define and a `pkg-config --static` wrapper, `go build`, `ldd`, and gateway `-h` all succeeded on linux/arm64. Runtime dependencies were libssl.so.3, libcrypto.so.3 and libc.so.6. This proves the fix direction on that earlier source snapshot; the final Docker image still needs to incorporate and validate the fixes. Logs: `build/refactor-review/linux-link.log`, `linux-link-after-compat.log`, and `linux-static-link.log`.

The shim and its two shared implementation headers were subsequently moved into the cgo package, keeping a single source for the C tests. Ordinary `go build` now compiles that package directly. Independent execution of the repository's `tools/gateway_shutdown_smoke.py` passed idle/TLS shutdown in 202.9/192.1 ms with exit 0. Requested explicit RDP_NEG_RSP/selected-SSL assertions and immediate EOF handling in its response reader before closing the test review.

## Final ARM64 image verification

Inspected image `sha256:add18a404ae2934172c6af0dacaacba9c619fdd1341526e4a191931f1f786081`: linux/arm64, runtime `-h` exits 0. A published-loopback-port smoke connected from the macOS SDL client. Host-to-VM UDP did not deliver video in the first setup; running the synthetic RTP encoder inside the gateway container isolated that transport issue. The final image then delivered 120 direct AVC420 picture PDUs at 640×360 and 118 Progressive picture PDUs at 1920×1080 for four-second 30 fps sources, with no decode errors and gateway exit 0. Containers and test processes were removed after each run. This validates the final FreeRDP 3.14 runtime paths; it is not Windows App physical latency evidence.

The retained agent, HID, control protocol and RTP packetizer files have no diff from `d7da465de9b13ad70798d13423940fe7cda99ce3`.
