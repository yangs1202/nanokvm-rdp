#!/usr/bin/env python3
"""Analyze CSV observations or validate a formal collector clock-calibration artifact."""
import argparse
import csv
import json
import math


def analyze(rows):
    samples = []
    for row in rows:
        source, before, after, error = (float(row[k]) for k in
            ('source_ms', 'capture_before_ms', 'capture_after_ms', 'sync_error_ms'))
        if not all(math.isfinite(x) for x in (source, before, after, error)):
            raise ValueError('Non-finite timestamp')
        if after < before or error < 0:
            raise ValueError('Invalid capture interval or sync error')
        # Includes source integer-ms rounding; does not include unmeasured drift.
        lo, hi = before-source-error-0.5, after-source+error+0.5
        samples.append({'frame_id': row['frame_id'], 'lower_ms': lo,
                        'upper_ms': hi, 'midpoint_ms': (lo+hi)/2,
                        'capture_window_ms': after-before})
    if not samples:
        raise ValueError('No samples')
    def quantile(key, q):
        values = sorted(s[key] for s in samples)
        return values[max(0, math.ceil(len(values)*q)-1)]
    return {'metric': 'visible source timestamp age; not physical display latency',
            'samples': len(samples), 'unique_frames': len({s['frame_id'] for s in samples}),
            'negative_intervals': sum(s['lower_ms'] < 0 for s in samples),
            'quantiles_ms': {name: {key: quantile(key,q) for key in
                ('lower_ms','midpoint_ms','upper_ms')} for name,q in
                [('p50',.5),('p95',.95),('p99',.99),('max',1)]},
            'max_capture_window_ms': max(s['capture_window_ms'] for s in samples),
            'observations': samples}


def finite(value, name):
    try:
        value = float(value)
    except (TypeError, ValueError):
        raise ValueError('%s is not numeric' % name)
    if not math.isfinite(value):
        raise ValueError('%s is not finite' % name)
    return value


def formal_clock_status(start, end, observations, interval_name, prefix):
    """Validate raw calibration fields rather than trusting collector summaries."""
    reasons = []
    if not isinstance(start, dict) or not isinstance(end, dict):
        return {'recalibration_status': 'missing_start_or_end_calibration',
                'drift_status': 'not_assessable',
                'bracketing_status': 'not_assessable'}, [prefix + '_calibration_missing']
    parsed = []
    for phase, calibration in (('start', start), ('end', end)):
        try:
            interval = calibration[interval_name]
            if not isinstance(interval, list) or len(interval) != 2:
                raise ValueError('interval is not a pair')
            lower, upper = (finite(value, prefix + ' ' + phase + ' interval')
                            for value in interval)
            offset = finite(calibration['offset_server_minus_' +
                            ('source_ms' if prefix == 'source' else 'local_ms')],
                            prefix + ' ' + phase + ' offset')
            error = finite(calibration['sync_error_ms'], prefix + ' ' + phase + ' uncertainty')
            if lower > upper or error < 0 or not lower <= offset <= upper:
                raise ValueError('invalid interval/offset/uncertainty')
            samples = calibration.get('samples')
            if not isinstance(samples, list) or not samples:
                raise ValueError('raw samples missing')
            if prefix == 'source':
                selected = calibration.get('selected_sample_index')
                if not any(sample.get('sample_index') == selected for sample in samples
                           if isinstance(sample, dict)):
                    raise ValueError('selected raw source sample missing')
                for sample in samples:
                    if not isinstance(sample, dict):
                        raise ValueError('raw source sample is not an object')
                    request, response = sample.get('request'), sample.get('response')
                    if not isinstance(request, dict) or not isinstance(response, dict):
                        raise ValueError('raw source request/response missing')
                    finite(request.get('local_before_ms'), 'raw source request time')
                    finite(sample.get('local_after_ms'), 'raw source response time')
                    if not isinstance(response.get('body'), str):
                        raise ValueError('raw source response body missing')
            parsed.append((lower, upper, calibration))
        except (KeyError, TypeError, ValueError):
            reasons.append(prefix + '_uncertainty_or_drift_ambiguous')
            return {'recalibration_status': 'start_and_end_calibration_recorded',
                    'drift_status': 'ambiguous',
                    'bracketing_status': 'ambiguous'}, reasons
    start_lower, start_upper, start_value = parsed[0]
    end_lower, end_upper, end_value = parsed[1]
    drift = [end_lower - start_upper, end_upper - start_lower]
    drift_status = ('no_drift_detected_within_combined_uncertainty'
                    if drift[0] <= 0 <= drift[1] else 'drift_detected')
    if drift_status == 'drift_detected':
        reasons.append(prefix + '_clock_drift_detected_requires_recalibration')
    try:
        first_before = min(finite(row['capture_before_server_ms'], 'capture before')
                           for row in observations)
        last_after = max(finite(row['capture_after_server_ms'], 'capture after')
                         for row in observations)
        start_done = finite(start_value['calibration_completed_server_ms'],
                            prefix + ' start completion')
        end_began = finite(end_value['calibration_started_server_ms'],
                            prefix + ' end start')
        bracketed = start_done <= first_before and end_began >= last_after
    except (KeyError, TypeError, ValueError):
        bracketed = False
        reasons.append(prefix + '_clock_not_bracketed')
    if not bracketed and prefix + '_clock_not_bracketed' not in reasons:
        reasons.append(prefix + '_clock_not_bracketed')
    return {
        'recalibration_status': 'start_and_end_calibration_recorded',
        'drift_status': drift_status,
        'drift_interval_server_minus_%s_ms' % ('source' if prefix == 'source' else 'local'): drift,
        'bracketing_status': 'bracketed' if bracketed else 'not_bracketed',
    }, reasons


def analyze_formal(artifact):
    """Return eligibility only; it deliberately does not manufacture a latency verdict."""
    reasons = []
    if not isinstance(artifact, dict):
        raise ValueError('collector artifact must be a JSON object')
    if artifact.get('collection_error'):
        reasons.append('collector_input_validation_failed')
    observations = artifact.get('observations')
    if not isinstance(observations, list) or not observations:
        reasons.append('collector_observations_missing')
        observations = []
    capture, capture_reasons = formal_clock_status(
        artifact.get('calibration_start'), artifact.get('calibration_end'), observations,
        'offset_interval_server_minus_local_ms', 'capture_host')
    reasons.extend(capture_reasons)
    source_section = artifact.get('source_clock_calibration')
    source_artifact = source_section.get('artifact') if isinstance(source_section, dict) else None
    source_calibrations = (source_artifact.get('source_calibrations', {})
                           if isinstance(source_artifact, dict) else {})
    if not isinstance(source_calibrations, dict):
        source_calibrations = {}
    source, source_reasons = formal_clock_status(
        source_calibrations.get('start', {}).get('report'),
        source_calibrations.get('end', {}).get('report'), observations,
        'offset_interval_server_minus_source_ms', 'source')
    reasons.extend(source_reasons)
    if not isinstance(source_artifact, dict) or source_artifact.get('run_id') != artifact.get('run_id'):
        reasons.append('source_clock_run_id_missing_or_mismatched')
    return {
        'kind': 'formal_video_latency_clock_eligibility',
        'run_id': artifact.get('run_id'),
        'eligible_for_formal_analysis': not reasons,
        'status': 'eligible_clock_calibration' if not reasons else 'non_eligible',
        'rejection_reasons': reasons,
        'capture_host_clock': capture,
        'source_window_clock': source,
        'metric_verdict': 'not_computed; clock eligibility is not a latency or A/B verdict',
    }


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('csv', nargs='?')
    parser.add_argument('--collector-artifact')
    args=parser.parse_args()
    if bool(args.csv) == bool(args.collector_artifact):
        parser.error('provide exactly one CSV input or --collector-artifact')
    if args.collector_artifact:
        with open(args.collector_artifact, encoding='utf-8') as file:
            print(json.dumps(analyze_formal(json.load(file)), indent=2))
    else:
        with open(args.csv,newline='') as file:
            print(json.dumps(analyze(list(csv.DictReader(file))),indent=2))
