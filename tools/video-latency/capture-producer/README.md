# ScreenCaptureKit capture producer

This macOS 14+ SwiftPM command-line producer implements the existing
`collector.py` JSONL contract. It captures one receiving **Windows App** window
with Apple's `SCScreenshotManager.captureSampleBuffer`, saves every returned
frame as PNG plus a metadata sidecar, and uses
`Vision.framework/VNRecognizeTextRequest` to read the displayed source timestamp
and `FRAME <id>` from that same image.

## Permission preflight

The capture target is the Windows App process/window, but Screen Recording
permission applies to the process hosting `capture-producer`. In **System
Settings > Privacy & Security > Screen Recording**, an operator must explicitly
allow the terminal or execution host that launches it (Terminal, iTerm, or the
Codex host as applicable). Quit that launching app first, enable it, then reopen
it if macOS asks. Do not enable Windows App: it is the capture target, not the
requesting process.

The producer never calls `CGRequestScreenCaptureAccess`, opens System Settings,
or changes permission. `--preflight` only calls
`CGPreflightScreenCaptureAccess`; it emits a JSON record and exits 3 when access
is absent:

```sh
swift run capture-producer --preflight
swift run capture-producer --list-windows
```

Only run `--list-windows` after preflight succeeds. If more than one Windows App
window exists, choose the receiving session by exact `--window-id`.

## Formal Vision OCR run

Vision OCR is the default; no metadata option is needed. It uses normalized,
overlapping timestamp and FRAME crops sized for `pattern.html`, accurate English
recognition with language correction disabled, and exact regular expressions.
Normalization trims surrounding whitespace only. It never substitutes lookalike
characters, guesses digits, or reuses an earlier frame's values.

Use CPython 3.10 or newer for `collector.py`. Earlier macOS CPython releases use
a process-local monotonic epoch and cannot share the producer's
`mach_absolute_time()` values.

```sh
swift run capture-producer \
  --window-id 1234 \
  --output-dir /absolute/path/to/raw-captures \
  --count 1000 --interval-ms 33 | \
python3 ../collector.py \
  --server-url http://127.0.0.1:8765/1 \
  --metadata /absolute/path/to/run-metadata.json \
  --run-id example --output /absolute/path/to/collector.json
```

Every attempt preserves the full PNG, timestamp crop, FRAME crop, ScreenCaptureKit
presentation timestamp, all Vision candidate strings/confidences/bounding boxes,
parse status, and selected values in `frame-NNNNNN.metadata.json`. If OCR or exact
parsing fails, that frame emits no collector JSONL record; its failure sidecar and
PNG remain. The producer continues through all requested captures, then writes
`session.json` with `capture_attempt_count`, `jsonl_emitted_count`, and
`ocr_failure_count` before exiting 6. It refuses a partial session unless all
sidecars exist and the emitted-plus-failed count matches all 1000 attempts, then
sets `attempts_reconciled_without_omissions=true`. Require that value for a
formal run; do not silently accept missing observations.

## Timestamp and calibration semantics

`capture_before_monotonic_ns` is sampled immediately before invoking
`SCScreenshotManager.captureSampleBuffer`; `capture_after_monotonic_ns` is sampled
as soon as its async completion returns a `CMSampleBuffer`. Both use
`mach_absolute_time()` converted by `mach_timebase_info`. PNG encoding and Vision
OCR happen after this interval. The sidecar preserves the returned CoreMedia
presentation timestamp, but never substitutes it for the local interval or the
source's displayed server timestamp. Wall clock and model-call time are never
used.

The producer records local run start/end samples but performs no Windows/source
to server calibration. The existing collector must call `/clock` and export its
start and end calibration around the same live run. Raw Windows frames, OCR
values, and collector calibration therefore must be collected together; the
producer cannot replace the remaining Windows-to-server calibration.

## Build and validation

```sh
./validate.sh
```

Validation builds the SwiftPM executable, generates a synthetic 1600×760 pattern
PNG, verifies successful Vision OCR and parser output, verifies a malformed
timestamp exits 6 without an invented value, checks the Swift/Python monotonic
timeline, and performs read-only permission preflight. The package requires
macOS 14 for `SCScreenshotManager`; independent-window filtering is available
from macOS 12.3.
