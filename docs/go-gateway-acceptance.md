# Go gateway refactor acceptance

## Fixed scope

- Baseline: `d7da465de9b13ad70798d13423940fe7cda99ce3`.
- Main task owns review and acceptance; GPT-5.6 Luna implements in an isolated worktree.
- Go owns agent control, RTP reception, orchestration and lifecycle. A C shim encapsulates FreeRDP peer, callbacks and codecs.
- Preserve C agent compatibility, CLI and RDP behavior, including direct AVC420 and bitmap fallback, input ordering, modifier handling, synchronize, release and reconnection.
- Do not restore the removed HTTP/WebSocket deployment.

## User-confirmed latency requirement

User revision: the previous 50 ms threshold is superseded. Both paths must take **less than 80 ms excluding network transit only, including the upper bound of measurement uncertainty**:

1. Screen: capture to actual presentation by the RDP client.
2. Input: input occurrence at the client to application of the key/mouse event by the target OS.

Report input-to-visible-response separately; it combines input, target processing and screen paths.
Gateway-only latency, a successful USB write, and an RDP frame acknowledgment are diagnostic measurements, not substitutes for these endpoints.

User-confirmed acceptance environment: **macOS Windows App → NanoKVM → target Mac**. FreeRDP client integration tests are supplementary and cannot replace this environment's final presentation/input evidence.

## Measurement contract

- Record commit, build options, machines, OS/client versions, negotiated codec, resolution, actual capture/presentation FPS and display refresh rate.
- Record each correlated event/frame ID and the timestamps at every measured boundary. Use monotonic clocks for durations within a process.
- Never subtract timestamps across hosts without a measured synchronization error bound. Do not subtract ping RTT or RTT/2 as though it were measured per-event one-way transit.
- Exclude only measured transit for the relevant network legs; retain encode/decode, local queues, cgo, target OS and presentation delays. Acceptance requires measured latency plus its conservative uncertainty upper bound below 80 ms. A network-inclusive upper bound below 80 ms is sufficient without subtracting network time; an inclusive result above the threshold is inconclusive about the network-excluded requirement.
- Record sample count, p50/p95/p99, maximum, missing samples, failed inputs, dropped frames and presentation FPS. An average below 80 ms is insufficient. An uncertainty interval crossing the threshold cannot prove acceptance; absent endpoint evidence is unverified.
- Exercise idle and moving screens, keyboard down/up and modifiers, pointer motion/buttons/wheels and simultaneous input/video. Separate direct AVC420 and supported bitmap fallback results. Explicitly distinguish single key events from multi-report Unicode/composed-text operations.
- Test reconnect, packet loss, slow clients and decoder failure for recovery and bounded queues. Report their interruption durations rather than silently excluding them from results.
- Use an instrumented client/target or an externally synchronized physical capture rig to observe actual OS application and presentation. A headless mock validates transport and protocol, not physical E2E latency.
- Browser-only target: the company Mac is attached to NanoKVM over HDMI/USB and cannot use SSH. Show an elapsed-millisecond timer and update sequence on that Mac; simultaneously film its original display and the Windows App display. Compare the two readings within each camera frame. This avoids cross-host clock subtraction, but directly measures display-to-display lag. Bound the difference between original-panel presentation and NanoKVM capture, camera exposure/rolling shutter, display scanout and timer update granularity before using it for capture-to-presentation acceptance. Millisecond digits are not proof of millisecond precision. Input acceptance still needs its own correlated endpoints.

## Review gates

1. Preserve existing tests and add meaningful Go protocol/lifecycle and shim integration checks.
2. Verify Go/C ownership: no retained movable Go pointers, callback-after-free, shutdown deadlocks, unbounded queues or reconnect input replay.
3. Preserve compressed H.264 dependencies; coalescing decoded frames must not discard required reference pictures.
4. Exercise a real FreeRDP connection and frame/input paths, not just mocked callbacks.
5. Require Linux production build evidence in addition to host build and race checks.
6. Keep performance acceptance open until the confirmed E2E requirement has evidence.

## Baseline evidence

Main task ran CMake with server and agent builds disabled in `build/refactor-baseline`; CTest passed all seven baseline tests on 2026-09-26. This proves those regression checks pass, not the latency requirement.
Existing logs expose partial timing only; capture-to-presentation and client-input-to-target-OS correlation are currently missing.
