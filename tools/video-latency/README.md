# Camera-free video latency probe

This probe measures **the age of a source draw timestamp visible in the received
Windows App image**. It is not HDMI capture-only delay, physical monitor latency,
or input latency. Existing gateway and agent binaries are unchanged.

## Run

```sh
python3 tools/video-latency/server.py --bind <Mac-LAN-IP>
```

For exploratory use, open the printed `/1` URL in the source Windows browser. Click **Start /
resync** and keep the page visible. For a formal collector run, instead open
`/1?run_id=<run-id>` and let the collector request start/end calibration; manual
resync or hiding that source tab invalidates the run. The server exposes only this page and its clock,
not repository files. Use a trusted LAN; stop the process after the measurement.
No Windows software installation or system clock change is required.

The page makes 20 clock exchanges and chooses the narrowest offset interval. For
client send/receive times t0/t3 and server receive/send times s1/s2, the offset is
bounded by `[s2-t3, s1-t0]`, assuming nonnegative transport delays. Its midpoint is
used for the source timestamp, and half its width is displayed as SYNC uncertainty.
This includes browser/server scheduling overhead, conservatively. No symmetric
network assumption is needed for the interval. Source timestamps use
`performance.now()` mapped to a stable server clock; server clock uses monotonic
elapsed time anchored at startup wall time.

Formal run reports preserve every raw `/1/clock` request/response, selected
offset/uncertainty, phase timestamps, run ID, and source drift/recalibration
state in the collector artifact. `ntp.yangs.sh` can be checked as a common external reference, but mere NTP
configuration does not demonstrate a small clock error. Direct clock calibration
is still required. Resync before and after a short run to assess drift; changing
calibration during a run invalidates comparisons across that change. Unmeasured
clock drift is not included in the displayed bound.

## Collect observations

The canvas shows a large server-timeline millisecond timestamp, frame sequence,
clock uncertainty, calibration age, and sequence/complement binary rows. Frames
are generated at approximately 30 Hz while requestAnimationFrame is active.
Actual rate may be lower because of refresh cadence and scheduling.

Capture the **receiving** Windows App window, not the original source page.
Record the timestamp and frame ID in each captured image, together with a local
capture interval. With Computer Use, bracket `app.getScreenshot()` with
`Date.now()` in the same tool execution; preserve both timestamps. Do not use
model response arrival time. This is a coarse diagnostic: it assumes the API
returns a newly acquired image inside that interval. Undocumented capture/cache
behavior remains an additional uncertainty. Dedicated capture with an OS-provided
frame timestamp is needed for a rigorous high-frequency benchmark.

The capture clock must match the server's stable timeline. Check local wall-clock
steps against the server startup anchor; discard affected runs. For a production
collector, use the same monotonic reference rather than Date.now().

CSV format:

```csv
source_ms,capture_before_ms,capture_after_ms,sync_error_ms,frame_id
```

```sh
python3 tools/video-latency/analyze.py observations.csv > result.json
```

Each observation gives bounds `[before-source-error-0.5,
after-source+error+0.5]` ms. The 0.5 ms covers rounding of the source stamp.
Negative bounds are reported, not clamped; investigate clock calibration, capture
clock mismatch, or transcription errors. Quantile bounds use nearest-rank order
statistics. Midpoints are estimates, not exact latency.

Repeated frame IDs represent visible staleness and remain in time-sampled data.
For first-arrival frame latency, use a continuous collector, retain first sightings,
and report the sampling interval separately. Sparse screenshots cannot measure
frame delivery FPS, exact freeze duration, or a reliable p99 from a few samples.

The measurement includes browser rendering, source composition, HDMI/capture,
encoder, video transport, gateway processing, RDP delivery, client composition,
and any age due to source refresh timing. It excludes physical pixel emission.
The changing test pattern can itself change codec workload. Record window size,
source resolution, video mode, bitrate, client and network conditions with results.

## Validation

server.py and analyze.py use Python standard library only. Analyze arithmetic can
be checked with synthetic timestamps; these are not live measurements. Verify
both real page visibility and successful calibration before collecting a baseline.

## Baselines and regression policy

See [benchmark procedure and acceptance criteria](../../docs/measurements/README.md)
for the preserved first baseline, required run metadata, and before/after comparison.
Refactoring must maintain or improve performance at equal quality. The current
coarse screenshot method cannot independently certify that requirement.
