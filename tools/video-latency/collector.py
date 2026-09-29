#!/usr/bin/env python3
"""Collect formal A/B video-latency observations on the server clock timeline.

The collector deliberately does not capture a window or OCR a frame.  A capture
producer with an explicitly documented timestamp API writes one JSON object per
line to stdin; this program calibrates that producer's monotonic clock to the
existing ``server.py`` clock and writes one immutable run artifact.
"""
import argparse
import json
import math
from pathlib import Path
import sys
import time
from urllib.parse import quote, urljoin
from urllib.error import HTTPError
from urllib.request import Request, urlopen


METADATA_DEFAULTS = {
    "source": {"resolution": None, "refresh_hz": None, "device": None,
               "browser": None},
    "video": {"codec": None, "bitrate_kbps": None, "gop": None,
              "fps": None},
    "client": {"application": None, "version": None, "platform": None,
               "window": None, "capture_api": None},
    "network": {"path": None, "transport": None, "rtt_ms": None,
                "loss_percent": None, "contention": None},
}


def local_ms():
    """The capture producer must use this same Python monotonic timebase."""
    return time.monotonic_ns() / 1_000_000


def require_finite(value, name):
    try:
        value = float(value)
    except (TypeError, ValueError):
        raise ValueError('%s must be a number' % name)
    if not math.isfinite(value):
        raise ValueError('%s must be finite' % name)
    return value


def merge_defaults(value, defaults):
    result = dict(defaults)
    for key, item in value.items():
        if isinstance(item, dict) and isinstance(defaults.get(key), dict):
            result[key] = merge_defaults(item, defaults[key])
        else:
            result[key] = item
    return result


def read_metadata(path):
    with open(path, encoding='utf-8') as file:
        value = json.load(file)
    if not isinstance(value, dict):
        raise ValueError('metadata file must contain a JSON object')
    for group in METADATA_DEFAULTS:
        if group in value and not isinstance(value[group], dict):
            raise ValueError('metadata.%s must be a JSON object' % group)
    return merge_defaults(value, METADATA_DEFAULTS)


def calibrate(server_url, samples, timeout):
    """Bound server_ms - local_monotonic_ms without assuming symmetric delay."""
    endpoint = urljoin(server_url.rstrip('/') + '/', 'clock')
    observations = []
    for index in range(samples):
        t0 = local_ms()
        request = Request(endpoint, headers={'Cache-Control': 'no-store'})
        with urlopen(request, timeout=timeout) as response:
            body = response.read()
        t3 = local_ms()
        payload = json.loads(body)
        received = require_finite(payload.get('received'), 'clock.received')
        sent = require_finite(payload.get('sent'), 'clock.sent')
        if sent < received:
            raise ValueError('clock response sent before received')
        # With non-negative request and response delays, server-local is here.
        lower, upper = sent - t3, received - t0
        if upper < lower:
            raise ValueError('impossible clock interval from server response')
        observations.append({
            'sample_index': index,
            'local_before_ms': t0,
            'local_after_ms': t3,
            'server_received_ms': received,
            'server_sent_ms': sent,
            'offset_interval_server_minus_local_ms': [lower, upper],
            'offset_server_minus_local_ms': (lower + upper) / 2,
            'sync_error_ms': (upper - lower) / 2,
            'round_trip_ms': t3 - t0,
        })
    chosen = min(observations, key=lambda item: item['sync_error_ms'])
    return {
        'clock': 'server.py monotonic-anchored timeline in milliseconds',
        'method': 'nonnegative-delay offset interval; narrowest interval selected',
        'sample_count': samples,
        'selected_sample_index': chosen['sample_index'],
        'offset_server_minus_local_ms': chosen['offset_server_minus_local_ms'],
        'sync_error_ms': chosen['sync_error_ms'],
        'offset_interval_server_minus_local_ms':
            chosen['offset_interval_server_minus_local_ms'],
        'calibration_started_server_ms':
            observations[0]['local_before_ms'] + chosen['offset_server_minus_local_ms'],
        'calibration_completed_server_ms':
            observations[-1]['local_after_ms'] + chosen['offset_server_minus_local_ms'],
        'samples': observations,
    }


def run_endpoint(server_url, run_id, action=None):
    base = server_url.rstrip('/') + '/runs/' + quote(run_id, safe='')
    return base if action is None else base + '/' + action


def request_json(url, timeout, method='GET', payload=None):
    data = None if payload is None else json.dumps(payload).encode('utf-8')
    request = Request(url, data=data, method=method,
                      headers={'Cache-Control': 'no-store',
                               'Content-Type': 'application/json'} if data else
                              {'Cache-Control': 'no-store'})
    with urlopen(request, timeout=timeout) as response:
        return json.loads(response.read())


def begin_source_run(args):
    return request_json(run_endpoint(args.server_url, args.run_id, 'start'),
                        args.timeout, method='POST', payload={})


def end_source_run(args):
    try:
        return request_json(run_endpoint(args.server_url, args.run_id, 'end'),
                            args.timeout, method='POST', payload={})
    except HTTPError as error:
        return {'request_error': {'status': error.code,
                                  'body': error.read().decode('utf-8', 'replace')}}


def wait_for_source_phase(args, phase):
    """Return the server's source-clock artifact, even when the phase is absent."""
    deadline = time.monotonic() + args.source_clock_wait
    latest = request_json(run_endpoint(args.server_url, args.run_id), args.timeout)
    while phase not in latest.get('source_calibrations', {}):
        if latest.get('status') == 'invalidated':
            return latest
        if time.monotonic() >= deadline:
            return latest
        time.sleep(min(.1, max(0, deadline - time.monotonic())))
        latest = request_json(run_endpoint(args.server_url, args.run_id), args.timeout)
    return latest


def clock_status(start, end, observations, interval_name, label):
    """Describe observed clock calibration facts without inferring a correction."""
    if not isinstance(start, dict) or not isinstance(end, dict):
        return {'recalibration_status': 'missing_start_or_end_calibration',
                'drift_status': 'not_assessable',
                'bracketing_status': 'not_assessable'}
    try:
        start_interval = start[interval_name]
        end_interval = end[interval_name]
        start_lower, start_upper = (require_finite(value, '%s start offset' % label)
                                    for value in start_interval)
        end_lower, end_upper = (require_finite(value, '%s end offset' % label)
                                for value in end_interval)
        if start_lower > start_upper or end_lower > end_upper:
            raise ValueError('reversed source offset interval')
        drift = [end_lower - start_upper, end_upper - start_lower]
        drift_status = ('no_drift_detected_within_combined_uncertainty'
                        if drift[0] <= 0 <= drift[1] else 'drift_detected')
        first_before = min(item['capture_before_server_ms'] for item in observations)
        last_after = max(item['capture_after_server_ms'] for item in observations)
        start_done = require_finite(start['calibration_completed_server_ms'],
                                    '%s start completion' % label)
        end_began = require_finite(end['calibration_started_server_ms'],
                                   '%s end start' % label)
        bracketed = start_done <= first_before and end_began >= last_after
        return {
            'recalibration_status': 'start_and_end_calibration_recorded',
            'drift_status': drift_status,
            'drift_interval_server_minus_%s_ms' %
            ('source' if label == 'source' else 'local'): drift,
            'bracketing_status': ('bracketed' if bracketed else 'not_bracketed'),
            '%s_start_calibration_completed_server_ms' % label: start_done,
            '%s_end_calibration_started_server_ms' % label: end_began,
        }
    except (KeyError, TypeError, ValueError):
        return {'recalibration_status': 'start_and_end_calibration_recorded',
                'drift_status': 'ambiguous',
                'bracketing_status': 'ambiguous'}


def source_clock_status(source_artifact, observations):
    calibrations = source_artifact.get('source_calibrations', {})
    return clock_status(calibrations.get('start', {}).get('report'),
                        calibrations.get('end', {}).get('report'), observations,
                        'offset_interval_server_minus_source_ms', 'source')


def parse_observation(value, line_number, calibration):
    required = ('capture_before_monotonic_ns', 'capture_after_monotonic_ns',
                'source_timestamp_server_ms', 'frame_id', 'capture_artifact')
    missing = [key for key in required if key not in value]
    if missing:
        raise ValueError('line %d missing %s' % (line_number, ', '.join(missing)))
    before_ns = require_finite(value['capture_before_monotonic_ns'],
                               'capture_before_monotonic_ns')
    after_ns = require_finite(value['capture_after_monotonic_ns'],
                              'capture_after_monotonic_ns')
    source_ms = require_finite(value['source_timestamp_server_ms'],
                               'source_timestamp_server_ms')
    if after_ns < before_ns:
        raise ValueError('line %d capture interval is negative' % line_number)
    if value['frame_id'] is None or str(value['frame_id']) == '':
        raise ValueError('line %d frame_id is empty' % line_number)
    if not isinstance(value['capture_artifact'], str) or not value['capture_artifact']:
        raise ValueError('line %d capture_artifact must be a nonempty path or ID' %
                         line_number)
    offset = calibration['offset_server_minus_local_ms']
    before_local_ms, after_local_ms = before_ns / 1_000_000, after_ns / 1_000_000
    return {
        'line_number': line_number,
        'frame_id': str(value['frame_id']),
        'source_timestamp_server_ms': source_ms,
        'capture_artifact': value['capture_artifact'],
        'capture_before_monotonic_ns': before_ns,
        'capture_after_monotonic_ns': after_ns,
        'capture_before_server_ms': before_local_ms + offset,
        'capture_after_server_ms': after_local_ms + offset,
        'capture_window_ms': after_local_ms - before_local_ms,
        'sync_error_ms': calibration['sync_error_ms'],
    }


def collect(args):
    metadata = read_metadata(args.metadata)
    source_started = begin_source_run(args)
    source_after_start = wait_for_source_phase(args, 'start')
    started = calibrate(args.server_url, args.calibration_samples, args.timeout)
    raw_observations, observations, first_seen, validation_errors = [], [], {}, []
    for line_number, raw in enumerate(sys.stdin, 1):
        raw = raw.rstrip('\r\n')
        if not raw:
            continue
        record = {'line_number': line_number, 'jsonl': raw,
                  'collector_received_monotonic_ns': time.monotonic_ns()}
        try:
            parsed = json.loads(raw)
            if not isinstance(parsed, dict):
                raise ValueError('input must be a JSON object')
            record['parsed'] = parsed
            observation = parse_observation(parsed, line_number, started)
        except (ValueError, TypeError, json.JSONDecodeError) as error:
            record['validation_error'] = str(error)
            raw_observations.append(record)
            validation_errors.append({'line_number': line_number,
                                      'error': str(error)})
            continue
        raw_observations.append(record)
        observation['observation_index'] = len(observations)
        observations.append(observation)
        first_seen.setdefault(observation['frame_id'], {
            'frame_id': observation['frame_id'],
            'observation_index': observation['observation_index'],
            'line_number': line_number,
            'capture_before_server_ms': observation['capture_before_server_ms'],
            'capture_after_server_ms': observation['capture_after_server_ms'],
        })
    ended = calibrate(args.server_url, args.calibration_samples, args.timeout)
    source_ending = end_source_run(args)
    source_artifact = wait_for_source_phase(args, 'end')
    source_artifact['collector_start_request'] = source_started
    source_artifact['collector_start_wait_snapshot'] = source_after_start
    source_artifact['collector_end_request'] = source_ending
    source_status = source_clock_status(source_artifact, observations)
    capture_status = clock_status(started, ended, observations,
                                  'offset_interval_server_minus_local_ms', 'capture_host')
    collection_error = None
    if validation_errors:
        collection_error = {'kind': 'invalid_jsonl_observation',
                            'validation_errors': validation_errors}
    elif not observations:
        collection_error = {'kind': 'no_jsonl_observations'}
    return {
        'schema_version': 2,
        'run_id': args.run_id,
        'kind': 'formal_video_latency_ab_collection',
        'status': ('collection_failed' if collection_error else
                   'collected_not_a_verdict'),
        'preliminary_baseline_comparison': 'prohibited',
        'server_url': args.server_url,
        'input_clock': 'Python time.monotonic_ns() from capture producer host',
        'input_contract': 'collector-README.md#jsonl-capture-input',
        'metadata': metadata,
        'calibration_start': started,
        'calibration_end': ended,
        'capture_host_clock_calibration_status': capture_status,
        'source_clock_calibration': {
            'artifact': source_artifact,
            'status': source_status,
        },
        'raw_observations': raw_observations,
        'observations': observations,
        'first_seen_frame_ids': list(first_seen.values()),
        'observation_count': len(observations),
        'unique_frame_count': len(first_seen),
        'collection_error': collection_error,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server-url', required=True,
                        help='server.py URL ending in /1, e.g. http://127.0.0.1:8765/1')
    parser.add_argument('--metadata', required=True,
                        help='JSON file containing source/video/client/network facts')
    parser.add_argument('--output', required=True, help='output JSON artifact path')
    parser.add_argument('--run-id', required=True)
    parser.add_argument('--calibration-samples', type=int, default=20)
    parser.add_argument('--timeout', type=float, default=5.0)
    parser.add_argument('--source-clock-wait', type=float, default=10.0,
                        help='seconds to wait for each source start/end calibration')
    args = parser.parse_args()
    if args.calibration_samples < 1:
        parser.error('--calibration-samples must be at least 1')
    if args.timeout <= 0:
        parser.error('--timeout must be positive')
    if args.source_clock_wait < 0:
        parser.error('--source-clock-wait must not be negative')
    try:
        artifact = collect(args)
        output = Path(args.output)
        if output.exists():
            raise ValueError('refusing to overwrite existing collector artifact: %s' %
                             output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(artifact, indent=2, sort_keys=True) + '\n',
                          encoding='utf-8')
        if artifact['collection_error']:
            print('collector: %s' % artifact['collection_error']['kind'],
                  file=sys.stderr)
            raise SystemExit(2)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print('collector: %s' % error, file=sys.stderr)
        raise SystemExit(2)


if __name__ == '__main__':
    main()
