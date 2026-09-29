#!/bin/sh
set -eu

cd "$(dirname "$0")"
swift build
swift run capture-producer --help > help.txt
printf '0\n' > help.exit
grep -q -- '--count defaults to' help.txt

set +e
.build/debug/capture-producer --output-dir /tmp/capture-producer-validation --count 999 \
  > formal-count-rejection.stdout 2> formal-count-rejection.stderr
formal_count_exit=$?
set -e
printf '%s\n' "$formal_count_exit" > formal-count-rejection.exit
if [ "$formal_count_exit" -ne 2 ]; then
  echo "validation failed: --count 999 exited $formal_count_exit, expected 2" >&2
  exit 1
fi
grep -q -- '--count must be at least 1000 for a formal run' formal-count-rejection.stderr

if [ -n "${COLLECTOR_PYTHON:-}" ]; then
  python="$COLLECTOR_PYTHON"
elif python3 -c 'import sys; raise SystemExit(sys.version_info < (3, 10))' 2>/dev/null; then
  python=python3
elif [ -x /opt/homebrew/bin/python3 ]; then
  python=/opt/homebrew/bin/python3
else
  echo "validation requires Python 3.10+ for a cross-process macOS monotonic clock" >&2
  exit 1
fi
if ! "$python" -c 'import sys; raise SystemExit(sys.version_info < (3, 10))' 2>/dev/null; then
  echo "validation requires COLLECTOR_PYTHON/Python 3.10+ for a cross-process macOS monotonic clock" >&2
  exit 1
fi

.build/debug/capture-producer \
  --make-synthetic-png synthetic-validation.png \
  --synthetic-timestamp 1750000000123 --synthetic-frame-id 481
.build/debug/capture-producer --ocr-png synthetic-validation.png > synthetic-ocr.json
printf '0\n' > synthetic-ocr.exit
.build/debug/capture-producer \
  --make-synthetic-png synthetic-ocr-failure.png \
  --synthetic-timestamp NOT_A_TIMESTAMP --synthetic-frame-id 481

"$python" - <<'PY'
import json
import subprocess
import time

before = time.monotonic_ns()
completed = subprocess.run(
    ['.build/debug/capture-producer', '--preflight'],
    capture_output=True,
    check=False,
    text=True,
)
after = time.monotonic_ns()
if completed.returncode not in (0, 3):
    raise SystemExit(f'unexpected preflight exit status: {completed.returncode}')
with open('preflight.exit', 'w', encoding='utf-8') as stream:
    stream.write(f'{completed.returncode}\n')
result = json.loads(completed.stdout)
def require(condition, message):
    if not condition:
        raise SystemExit(f'validation failed: {message}')

require(result['permission_api'] == 'CGPreflightScreenCaptureAccess', 'unexpected permission API')
require(result['permission_request_performed'] is False, 'preflight requested permission')
require(isinstance(result['screen_recording_authorized'], bool), 'authorization result is not boolean')
require(before <= result['local_monotonic_ns'] <= after, 'preflight monotonic timestamp was outside invocation interval')
require('System Settings > Privacy & Security > Screen Recording' in result['permission_instructions'], 'missing Screen Recording diagnostic')
with open('preflight.json', 'w', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2, sort_keys=True)
    stream.write('\n')

with open('synthetic-ocr.json', encoding='utf-8') as stream:
    ocr = json.load(stream)
require(ocr['status'] == 'ok', 'known OCR fixture did not parse')
require(ocr['source_timestamp_server_ms'] == 1750000000123, 'OCR timestamp differs from fixture')
require(ocr['frame_id'] == '481', 'OCR frame ID differs from fixture')
require(ocr['timestamp']['candidates'], 'OCR raw timestamp candidates were not retained')
require(ocr['frame']['candidates'], 'OCR raw frame candidates were not retained')

failed = subprocess.run(
    ['.build/debug/capture-producer', '--ocr-png', 'synthetic-ocr-failure.png'],
    capture_output=True,
    check=False,
    text=True,
)
require(failed.returncode == 6, f'OCR failure fixture exit was {failed.returncode}, expected 6')
with open('synthetic-ocr-failure.exit', 'w', encoding='utf-8') as stream:
    stream.write(f'{failed.returncode}\n')
failure = json.loads(failed.stdout)
require(failure['status'] == 'ocr_parse_failed', 'OCR failure fixture reported success')
require('source_timestamp_server_ms' not in failure, 'OCR failure emitted a timestamp')
require('frame_id' not in failure, 'OCR failure emitted a frame ID')
require(failure['timestamp']['candidates'], 'OCR failure did not retain raw observations')
with open('synthetic-ocr-failure.json', 'w', encoding='utf-8') as stream:
    json.dump(failure, stream, indent=2, sort_keys=True)
    stream.write('\n')
PY

status=$($python -c 'import json; print(0 if json.load(open("preflight.json"))["screen_recording_authorized"] else 3)')
echo "build=passed help=passed vision_ocr=passed ocr_failure=passed preflight=passed permission_exit=$status monotonic_crosscheck=passed"
if [ "$status" = 3 ]; then
  echo "Screen Recording is absent as expected for permission-free validation; enable the launching host in System Settings > Privacy & Security > Screen Recording for live capture."
else
  echo "Screen Recording is available; validation performed no permission request."
fi
