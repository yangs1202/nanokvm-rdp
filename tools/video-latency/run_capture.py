#!/usr/bin/env python3
"""Run one collector-first capture with separate wall and no-progress timeouts.

This wrapper deliberately preserves every run directory. It starts the collector
before the source start calibration, retries the server's initial 404 while the
collector registers the run, then tees producer JSONL to both the collector and
an immutable local file. Vision OCR is still performed by the producer; a later
capture/OCR split can consume the same raw PNG/sidecar directory without changing
the collector contract.
"""

import argparse
import json
import queue
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import urlopen


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-id', required=True)
    parser.add_argument('--run-dir', required=True, type=Path)
    parser.add_argument('--server-url', required=True)
    parser.add_argument('--metadata', required=True, type=Path)
    parser.add_argument('--pattern-version')
    parser.add_argument('--pattern-sha256')
    parser.add_argument('--producer', required=True, type=Path)
    parser.add_argument('--collector', required=True, type=Path)
    parser.add_argument('--python', default=sys.executable)
    parser.add_argument('--window-id', required=True)
    parser.add_argument('--count', type=int, default=1000)
    parser.add_argument('--interval-ms', type=int, default=33)
    parser.add_argument('--warmup-seconds', type=float, default=30.0)
    parser.add_argument('--wall-timeout-seconds', type=float, default=900.0)
    parser.add_argument('--no-progress-timeout-seconds', type=float, default=60.0)
    parser.add_argument('--source-clock-wait-seconds', type=float, default=30.0)
    args = parser.parse_args()
    if args.count < 1000:
        parser.error('--count must be at least 1000')
    for name in ('interval_ms', 'warmup_seconds', 'wall_timeout_seconds',
                 'no_progress_timeout_seconds', 'source_clock_wait_seconds'):
        if getattr(args, name) < 0:
            parser.error(f'--{name.replace("_", "-")} must not be negative')
    if args.wall_timeout_seconds <= 0 or args.no_progress_timeout_seconds <= 0:
        parser.error('wall and no-progress timeouts must be positive')
    return args


def read_run_state(url, timeout=3.0):
    with urlopen(url, timeout=timeout) as response:
        return json.load(response)


def preflight_collector_python(executable):
    resolved = shutil.which(executable)
    if resolved is None:
        raise SystemExit(f'collector Python not found: {executable}')
    resolved = str(Path(resolved).absolute())
    probe = ('import json,sys; print(json.dumps({"platform":sys.platform,'
             '"version":list(sys.version_info[:3]),"executable":sys.executable}))')
    try:
        result = subprocess.run([resolved, '-c', probe], check=True,
                                capture_output=True, text=True, timeout=10)
        runtime = json.loads(result.stdout)
        if runtime['platform'] == 'darwin' and tuple(runtime['version']) < (3, 10):
            raise SystemExit(
                f'collector Python {resolved} must be >=3.10 on macOS: older '
                'Python monotonic clocks are not shared across processes; '
                'use --python /opt/homebrew/bin/python3')
    except (OSError, subprocess.SubprocessError, ValueError, KeyError, TypeError) as exc:
        raise SystemExit(f'collector Python preflight failed: {exc}') from exc
    runtime['invoked_executable'] = resolved
    return runtime


def terminate_process(process, label):
    if process is None or process.poll() is not None:
        return process.returncode if process is not None else None
    print(f'terminating_{label}', flush=True)
    process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        print(f'killing_{label}', flush=True)
        process.kill()
        process.wait(timeout=10)
    return process.returncode


def main():
    args = parse_args()
    runtime = preflight_collector_python(args.python)
    args.python = runtime['invoked_executable']
    run_dir = args.run_dir
    if run_dir.exists():
        raise SystemExit(f'refusing to overwrite existing run directory: {run_dir}')
    run_dir.mkdir(parents=True)
    raw_dir = run_dir / 'raw-captures'
    raw_dir.mkdir()
    metadata_path = run_dir / 'metadata.json'
    shutil.copy2(args.metadata, metadata_path)
    collector_command = [
        args.python, str(args.collector.absolute()), '--server-url', args.server_url,
        '--metadata', str(metadata_path.absolute()), '--run-id', args.run_id,
        '--output', str((run_dir / 'collector.json').absolute()),
        '--source-clock-wait', str(args.source_clock_wait_seconds)]
    metadata = json.loads(metadata_path.read_text(encoding='utf-8'))
    if args.pattern_version:
        metadata.setdefault('formal_measurement', {})['pattern_version'] = args.pattern_version
    if args.pattern_sha256:
        metadata['pattern_source_sha256'] = args.pattern_sha256
    metadata['capture_execution'] = {
        'wrapper_command': [str(Path(sys.executable).absolute()),
                            str(Path(sys.argv[0]).absolute()), *sys.argv[1:]],
        'collector_command': collector_command,
        'collector_python': runtime,
    }
    metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + '\n', encoding='utf-8')

    collector = None
    producer = None
    collector_rc = None
    producer_rc = None
    error = None
    forwarded = 0
    source_start_confirmed_at = None
    producer_started_at = None
    producer_finished_at = None
    run_started_at = time.time()
    collector_stdout = (run_dir / 'collector.stdout').open('wb')
    collector_stderr = (run_dir / 'collector.stderr').open('wb')
    producer_stderr = None
    producer_jsonl = None
    try:
        collector = subprocess.Popen(
            collector_command,
            stdin=subprocess.PIPE, stdout=collector_stdout, stderr=collector_stderr)
        print(f'collector_started pid={collector.pid}', flush=True)

        state = None
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            if collector.poll() is not None:
                raise RuntimeError(
                    f'collector exited before source start: rc={collector.returncode}')
            try:
                state = read_run_state(f'{args.server_url}/runs/{args.run_id}')
            except HTTPError as exc:
                if exc.code != 404:
                    raise
                state = None
            except (OSError, URLError, TimeoutError):
                state = None
            if (state and state.get('status') == 'active' and
                    'start' in state.get('source_calibrations', {})):
                source_start_confirmed_at = time.time()
                print('source_start_confirmed', flush=True)
                break
            time.sleep(.25)
        else:
            raise TimeoutError('source start calibration did not complete')

        print(f'warmup_started seconds={args.warmup_seconds}', flush=True)
        time.sleep(args.warmup_seconds)
        if collector.poll() is not None:
            raise RuntimeError(f'collector exited during warmup: rc={collector.returncode}')

        producer_stderr = (run_dir / 'producer.stderr').open('wb')
        producer_jsonl = (run_dir / 'producer.jsonl').open('wb')
        producer = subprocess.Popen(
            [str(args.producer), '--window-id', args.window_id,
             '--output-dir', str(raw_dir), '--count', str(args.count),
             '--interval-ms', str(args.interval_ms)],
            stdout=subprocess.PIPE, stderr=producer_stderr)
        producer_started_at = time.time()
        print(f'producer_started pid={producer.pid}', flush=True)

        lines = queue.Queue()

        def pump_stdout():
            try:
                for line in producer.stdout:
                    lines.put(line)
            finally:
                lines.put(None)

        reader = threading.Thread(target=pump_stdout, name='producer-stdout', daemon=True)
        reader.start()
        wall_deadline = time.monotonic() + args.wall_timeout_seconds
        progress_deadline = time.monotonic() + args.no_progress_timeout_seconds
        while True:
            now = time.monotonic()
            if now >= wall_deadline:
                raise TimeoutError('producer wall timeout expired')
            if now >= progress_deadline:
                raise TimeoutError('producer no-progress timeout expired')
            try:
                line = lines.get(timeout=min(1.0, wall_deadline - now,
                                            progress_deadline - now))
            except queue.Empty:
                continue
            if line is None:
                break
            producer_jsonl.write(line)
            producer_jsonl.flush()
            if collector.stdin is None:
                raise RuntimeError('collector stdin unexpectedly closed')
            collector.stdin.write(line)
            collector.stdin.flush()
            forwarded += 1
            progress_deadline = time.monotonic() + args.no_progress_timeout_seconds
            if forwarded % 100 == 0:
                print(f'forwarded_lines={forwarded}', flush=True)
        reader.join(timeout=5)
        producer_rc = producer.wait(timeout=20)
        producer_finished_at = time.time()
        print(f'producer_finished rc={producer_rc} lines={forwarded}', flush=True)
    except Exception as exc:  # preserve the run result in finally below
        error = f'{type(exc).__name__}: {exc}'
        print(f'run_error={error}', file=sys.stderr, flush=True)
    finally:
        if producer is not None:
            producer_rc = terminate_process(producer, 'producer')
        if producer_jsonl is not None:
            producer_jsonl.close()
        if producer_stderr is not None:
            producer_stderr.close()
        if collector is not None and collector.stdin is not None:
            try:
                collector.stdin.close()
            except OSError:
                pass
        if collector is not None:
            try:
                collector_rc = collector.wait(timeout=120)
            except subprocess.TimeoutExpired:
                collector_rc = terminate_process(collector, 'collector')
            print(f'collector_finished rc={collector_rc}', flush=True)
        collector_stdout.close()
        collector_stderr.close()

    if error is not None or collector_rc != 0 or producer_rc not in (0, 6):
        run_status = 'incomplete'
        exit_code = 1
    elif producer_rc == 6:
        run_status = 'completed_with_ocr_failures'
        exit_code = 6
    else:
        run_status = 'complete'
        exit_code = 0
    status = {
        'collector': collector_rc,
        'error': error,
        'finished_at_unix': time.time(),
        'forwarded_lines': forwarded,
        'producer': producer_rc,
        'run_id': args.run_id,
        'status': run_status,
    }
    (run_dir / 'exit-status.json').write_text(
        json.dumps(status, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    (run_dir / 'timing.json').write_text(json.dumps({
        'capture_elapsed_seconds': (producer_finished_at - producer_started_at)
        if producer_finished_at and producer_started_at else None,
        'finished_at_unix': time.time(),
        'no_progress_timeout_seconds': args.no_progress_timeout_seconds,
        'producer_finished_at_unix': producer_finished_at,
        'producer_started_at_unix': producer_started_at,
        'producer_wall_timeout_seconds': args.wall_timeout_seconds,
        'source_start_confirmed_at_unix': source_start_confirmed_at,
        'warmup_elapsed_seconds': (producer_started_at - source_start_confirmed_at)
        if producer_started_at and source_start_confirmed_at else None,
        'warmup_seconds': args.warmup_seconds,
    }, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    print('exit_status=' + json.dumps(status, sort_keys=True), flush=True)
    return exit_code


if __name__ == '__main__':
    raise SystemExit(main())
