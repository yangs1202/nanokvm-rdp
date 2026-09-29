#!/usr/bin/env python3
"""Serve the latency pattern, clock, and in-memory source-clock run artifacts."""
import argparse
import json
import math
import re
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Lock
import time
from urllib.parse import unquote, urlparse


RUN_ID = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$')


def require_finite(value, name):
    try:
        value = float(value)
    except (TypeError, ValueError):
        raise ValueError('%s must be a number' % name)
    if not math.isfinite(value):
        raise ValueError('%s must be finite' % name)
    return value


class RunRegistry:
    """Keep reports only long enough for the collector to write its run artifact."""

    def __init__(self, now_ms):
        self.now_ms = now_ms
        self.runs = {}
        self.lock = Lock()

    @staticmethod
    def validate_run_id(run_id):
        if not isinstance(run_id, str) or not RUN_ID.fullmatch(run_id):
            raise ValueError('invalid run_id')
        return run_id

    def start(self, run_id):
        run_id = self.validate_run_id(run_id)
        with self.lock:
            if run_id in self.runs:
                raise ValueError('run_id already exists')
            now = self.now_ms()
            self.runs[run_id] = {
                'schema_version': 1,
                'run_id': run_id,
                'clock': 'server.py monotonic-anchored timeline in milliseconds',
                'run_started_server_ms': now,
                'status': 'start_requested',
                'source_calibrations': {},
            }
            return self.snapshot_locked(run_id)

    def end(self, run_id):
        with self.lock:
            run = self.get_locked(run_id)
            if run['status'] == 'complete':
                raise ValueError('run is already complete')
            run['run_end_requested_server_ms'] = self.now_ms()
            run['status'] = 'end_requested'
            return self.snapshot_locked(run_id)

    def invalidate(self, run_id, reason):
        with self.lock:
            run = self.get_locked(run_id)
            if run['status'] == 'complete':
                raise ValueError('run is already complete')
            if not isinstance(reason, str) or not reason:
                raise ValueError('invalidation reason is required')
            run['invalidated_server_ms'] = self.now_ms()
            run['invalidation'] = {'reason': reason}
            run['status'] = 'invalidated'
            return self.snapshot_locked(run_id)

    def get(self, run_id):
        with self.lock:
            self.get_locked(run_id)
            return self.snapshot_locked(run_id)

    def report(self, run_id, report):
        with self.lock:
            run = self.get_locked(run_id)
            if not isinstance(report, dict):
                raise ValueError('calibration report must be a JSON object')
            if report.get('run_id') != run_id:
                raise ValueError('calibration report run_id does not match URL')
            phase = report.get('phase')
            expected = ('start' if run['status'] == 'start_requested' else
                        'end' if run['status'] == 'end_requested' else None)
            if phase != expected:
                raise ValueError('calibration phase is not requested for this run')
            self.validate_calibration_report(report)
            run['source_calibrations'][phase] = {
                'server_received_ms': self.now_ms(),
                # Preserve browser request/response samples verbatim.
                'report': report,
            }
            run['status'] = 'active' if phase == 'start' else 'complete'
            return self.snapshot_locked(run_id)

    def get_locked(self, run_id):
        run_id = self.validate_run_id(run_id)
        if run_id not in self.runs:
            raise ValueError('unknown run_id')
        return self.runs[run_id]

    def snapshot_locked(self, run_id):
        return json.loads(json.dumps(self.runs[run_id]))

    @staticmethod
    def validate_calibration_report(report):
        for name in ('calibration_started_server_ms',
                     'calibration_completed_server_ms',
                     'offset_server_minus_source_ms',
                     'sync_error_ms'):
            require_finite(report.get(name), 'report.%s' % name)
        interval = report.get('offset_interval_server_minus_source_ms')
        if (not isinstance(interval, list) or len(interval) != 2 or
                require_finite(interval[0], 'report.offset interval lower') >
                require_finite(interval[1], 'report.offset interval upper')):
            raise ValueError('invalid report offset interval')
        if require_finite(report['sync_error_ms'], 'report.sync_error_ms') < 0:
            raise ValueError('report.sync_error_ms must not be negative')
        samples = report.get('samples')
        if not isinstance(samples, list) or not samples:
            raise ValueError('report.samples must be a nonempty array')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bind', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8765)
    args = parser.parse_args()
    page = Path(__file__).with_name('pattern.html').read_bytes()
    origin_ms = time.time_ns() / 1e6
    origin_ns = time.monotonic_ns()

    def now_ms():
        return origin_ms + (time.monotonic_ns() - origin_ns) / 1e6

    runs = RunRegistry(now_ms)

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            received = now_ms()
            path = urlparse(self.path).path
            if path in ('/1', '/1/'):
                self.send_bytes(200, page, 'text/html; charset=utf-8')
            elif path == '/1/clock':
                self.send_json(200, {'received': received, 'sent': now_ms()})
            elif path.startswith('/1/runs/'):
                self.run_get(path)
            else:
                self.send_error(404)

        def do_POST(self):
            path = urlparse(self.path).path
            if not path.startswith('/1/runs/'):
                self.send_error(404)
                return
            try:
                parts = [unquote(part) for part in path.split('/') if part]
                if len(parts) != 4 or parts[0:2] != ['1', 'runs']:
                    self.send_error(404)
                    return
                run_id, action = parts[2], parts[3]
                if action == 'start':
                    self.send_json(201, runs.start(run_id))
                elif action == 'end':
                    self.send_json(200, runs.end(run_id))
                elif action == 'invalidate':
                    body = self.read_json_body()
                    self.send_json(200, runs.invalidate(run_id, body.get('reason')))
                elif action == 'calibrations':
                    self.send_json(201, runs.report(run_id, self.read_json_body()))
                else:
                    self.send_error(404)
            except (UnicodeDecodeError, ValueError, json.JSONDecodeError) as error:
                self.send_json(400, {'error': str(error)})

        def run_get(self, path):
            try:
                parts = [unquote(part) for part in path.split('/') if part]
                if len(parts) == 3 and parts[0:2] == ['1', 'runs']:
                    self.send_json(200, runs.get(parts[2]))
                else:
                    self.send_error(404)
            except (UnicodeDecodeError, ValueError) as error:
                self.send_json(404, {'error': str(error)})

        def read_json_body(self):
            length = int(self.headers.get('Content-Length', '0'))
            if length < 1 or length > 2_000_000:
                raise ValueError('invalid calibration report size')
            return json.loads(self.rfile.read(length))

        def send_json(self, status, value):
            self.send_bytes(status, json.dumps(value, separators=(',', ':')).encode(),
                            'application/json')

        def send_bytes(self, status, body, kind):
            self.send_response(status)
            self.send_header('Content-Type', kind)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, fmt, *args):
            pass

    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    actual_port = server.server_address[1]
    print(json.dumps({'url': f'http://{args.bind}:{actual_port}/1',
                      'origin_ms': origin_ms}), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
