# Formal A/B continuous collector

`collector.py` is the formal-run recorder for this directory's existing
`server.py` pattern. It does **not** create a screenshot, access a macOS window,
or perform OCR. It converts capture-producer timestamps from the producer's
`time.monotonic_ns()` timeline onto the exact monotonic-anchored server timeline
returned by `server.py /1/clock`.

It starts and ends a matching source-page run through `server.py`; a source page
opened with `?run_id=<run-id>` automatically performs browser-to-server
calibration for both boundaries. The collector stores the server run artifact,
including raw browser `/1/clock` request/response samples, selected offset,
uncertainty, phase timestamps, run ID, and drift/bracketing/recalibration status.
It separately retains capture-host (`time.monotonic_ns()`) calibrations: its
`sync_error_ms` is never source-window uncertainty or end drift. Each accepted line is stored
unchanged in `raw_observations`; normalized observations include the source
timestamp, capture interval before/after in both clock domains, sync error,
capture artifact reference, and first sighting for each frame ID. Repeated frame
IDs remain observations; `first_seen_frame_ids` is a separate delivery/FPS aid.

This is a collector artifact, not a latency analysis or an A/B verdict. Its
`preliminary_baseline_comparison` explicitly prohibits treating the preserved
20-screenshot preliminary run as comparable formal data.

## Capture API blocker

There is no repository-owned Computer Use, macOS ScreenCaptureKit, or Windows
App capture integration here, and this tool must not invent one. In particular,
the existing documentation's illustrative `app.getScreenshot()` call is not an
API contract for this collector and its response timing is not a verified frame
acquisition timestamp.

Live formal collection is blocked until the operator supplies a capture producer
that has permission to capture the receiving Windows App window and can emit a
verified **local monotonic** before/after interval for every captured frame. The
producer and collector must run on the same host and use the same
`time.monotonic_ns()` clock. If the available capture API only supplies wall time,
response-arrival time, cached images, or undocumented frame timing, do not use it
for a formal run; record that exact limitation and leave the A/B decision pending.

## JSONL capture input

Start `server.py`, then open the source page with the exact run ID before starting
the collector: `http://<host>:8765/1?run_id=<run-id>`. Keep that tab visible.
The collector requests source start calibration, waits for it before reading JSONL,
then requests and waits for source end calibration after stdin closes. End stdin
(Ctrl-D) to take end calibration and write the artifact. Empty lines are ignored. Every
nonempty line must be a JSON object with these required fields:

```json
{
  "capture_before_monotonic_ns": 123456789000000,
  "capture_after_monotonic_ns": 123456789034000,
  "source_timestamp_server_ms": 1750000000123,
  "frame_id": 481,
  "capture_artifact": "/absolute/path/to/receiving-window-frame.png"
}
```

`capture_before_monotonic_ns` and `capture_after_monotonic_ns` bracket the
capture API call, on the producer host. `source_timestamp_server_ms` and
`frame_id` are read from that captured receiving-window frame; a producer may
use validated OCR, but the unchanged input line makes that method auditable.
`capture_artifact` is a nonempty path or immutable external capture ID. The
collector preserves it but does not create, inspect, hash, or claim the artifact
exists.

The capture-host clock conversion is bounded without a symmetric-network assumption. For local
request/response times `t0/t3` and server `received/sent` times `s1/s2`, the
server-minus-local offset is `[s2-t3, s1-t0]`; the midpoint is used and half the
width is stored as `sync_error_ms`. Capture server time is local monotonic time
plus that selected offset. Start/end calibrations must both be retained; their
difference exposes capture-host drift or path changes. The source report uses the
same interval math independently. Its start completion must precede the first
capture interval and its end start must follow the last one; otherwise it does
not bracket the run. Manual resync, tab-hidden, or calibration failure during a
formal run records invalidation and must not be continued as the same run.

Malformed JSONL does not discard input evidence. The collector consumes stdin,
records every raw nonempty line and validation error in an immutable artifact
with `status: collection_failed`, then exits nonzero. It still refuses to
overwrite any existing artifact, including historical baselines.

## Metadata and invocation

Create a per-run metadata JSON from
[`docs/measurements/2026-09-29-video-latency-formal-manifest.json`](../../docs/measurements/2026-09-29-video-latency-formal-manifest.json).
Keep unknown values `null`; never infer negotiated codec, bitrate, GOP, source
resolution, client version, or network characteristics. The collector requires a
metadata JSON object and preserves extra fields, while filling missing fields in
the required source/video/client/network groups with `null`.

```sh
python3 tools/video-latency/server.py --bind <capture-host-LAN-IP>

capture-producer --window "Windows App" --emit-formal-jsonl | \
  python3 tools/video-latency/collector.py \
    --server-url http://<capture-host-LAN-IP>:8765/1 \
    --metadata docs/measurements/<run-id>-metadata.json \
    --run-id <run-id> \
    --output docs/measurements/<run-id>-collector.json

python3 tools/video-latency/analyze.py \
  --collector-artifact docs/measurements/<run-id>-collector.json
```

`capture-producer` above is intentionally a placeholder, not a command supplied
by this repository. Preserve the producer's actual command, capture API/version,
permissions, and every capture artifact outside this collector JSON. Run each A
and B condition after 30-second warm-up for at least 60 seconds, with matched
quality and metadata; alternate order and retain at least three pairs. Analyze
formal runs separately from the preliminary 20 screenshots.

The formal analyzer reports `eligible_for_formal_analysis: false` when either
capture-host or source-window start/end calibration is missing, its raw interval
or uncertainty is ambiguous, clock drift is detected without a correction, or
capture intervals are not bracketed. Clock eligibility is not a latency or A/B
performance verdict.
