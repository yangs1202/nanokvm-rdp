#!/usr/bin/env python3
"""Analyze CSV observations or validate a formal collector clock-calibration artifact."""
import argparse
import csv
import json
import math
from pathlib import Path


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
    return {'kind': 'preliminary_csv_fixture_validation',
            'status': 'preliminary_not_formal',
            'metric': 'visible source timestamp age; not physical display latency',
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


CONDITION_FIELDS = {
    'source': ('resolution', 'refresh_hz', 'device', 'browser'),
    'video': ('codec', 'bitrate_kbps', 'gop', 'fps'),
    'client': ('application', 'version', 'platform', 'window', 'capture_api'),
    'network': ('path', 'transport', 'rtt_ms', 'loss_percent', 'contention'),
}
FORMAL_FIELDS = ('warmup_elapsed_ms', 'condition_id', 'pattern_version',
                 'workload', 'capture_timestamp_semantics_verified',
                 'source_geometry', 'receiver_geometry', 'capture_geometry',
                 'capture_settings', 'video_quality_verified', 'actual_gop')
UNKNOWN_TEXT = {'unknown', 'n/a', 'na', 'none', 'null', 'unset', 'unverified'}


def is_present(value):
    return value is not None and (not isinstance(value, str) or
                                  bool(value.strip()) and value.strip().lower() not in UNKNOWN_TEXT)


def positive_number(value):
    try:
        return finite(value, 'metadata value') > 0
    except ValueError:
        return False


def nonnegative_number(value):
    try:
        return finite(value, 'metadata value') >= 0
    except ValueError:
        return False


def recorded_structure(value):
    """Allow concrete strings or nonempty structured settings, never unknown placeholders."""
    if isinstance(value, dict):
        return bool(value) and all(recorded_structure(item) for item in value.values())
    if isinstance(value, (list, tuple)):
        return bool(value) and all(recorded_structure(item) for item in value)
    return is_present(value)


def condition_value_valid(group, field, value):
    if group == 'network' and field == 'contention':
        return isinstance(value, bool) or (isinstance(value, str) and is_present(value))
    if group == 'network' and field in ('rtt_ms', 'loss_percent'):
        return nonnegative_number(value)
    if group == 'video' and field in ('bitrate_kbps', 'gop', 'fps'):
        return positive_number(value)
    if group == 'source' and field == 'refresh_hz':
        return positive_number(value)
    return isinstance(value, str) and is_present(value)


def metadata_status(metadata):
    """Require recorded conditions rather than treating collector defaults as facts."""
    missing = []
    if not isinstance(metadata, dict):
        return ['metadata_missing'], {}
    for group, fields in CONDITION_FIELDS.items():
        values = metadata.get(group)
        for field in fields:
            value = values.get(field) if isinstance(values, dict) else None
            if not is_present(value):
                missing.append('condition_metadata_missing:%s.%s' % (group, field))
            elif not condition_value_valid(group, field, value):
                missing.append('condition_metadata_invalid:%s.%s' % (group, field))
    formal = metadata.get('formal_measurement')
    if not isinstance(formal, dict):
        return missing + ['formal_measurement_metadata_missing'], {}
    for field in FORMAL_FIELDS:
        if not is_present(formal.get(field)):
            missing.append('formal_measurement_missing:%s' % field)
    for field in ('condition_id', 'pattern_version', 'workload', 'source_geometry',
                  'receiver_geometry', 'capture_geometry'):
        if field in formal and (not isinstance(formal[field], str) or not is_present(formal[field])):
            missing.append('formal_measurement_invalid:%s' % field)
    if 'capture_settings' in formal and not recorded_structure(formal['capture_settings']):
        missing.append('formal_measurement_invalid:capture_settings')
    try:
        warmup = finite(formal.get('warmup_elapsed_ms'), 'warmup_elapsed_ms')
        if warmup < 30_000:
            missing.append('warmup_less_than_30s')
    except ValueError:
        if 'formal_measurement_missing:warmup_elapsed_ms' not in missing:
            missing.append('warmup_elapsed_ms_ambiguous')
    if formal.get('capture_timestamp_semantics_verified') is not True:
        missing.append('capture_timestamp_semantics_not_verified')
    if formal.get('video_quality_verified') is not True:
        missing.append('video_quality_not_verified')
    if not positive_number(formal.get('actual_gop')):
        missing.append('actual_gop_ambiguous')
    elif positive_number(metadata.get('video', {}).get('gop')) and \
            finite(formal['actual_gop'], 'actual_gop') != finite(metadata['video']['gop'], 'video.gop'):
        missing.append('actual_gop_does_not_match_video_metadata')
    return missing, formal


def valid_observations(observations):
    valid, errors = [], []
    for index, observation in enumerate(observations):
        if not isinstance(observation, dict):
            errors.append('invalid_observation:%d:not_object' % index)
            continue
        try:
            before = finite(observation.get('capture_before_server_ms'), 'capture before')
            after = finite(observation.get('capture_after_server_ms'), 'capture after')
            source = finite(observation.get('source_timestamp_server_ms'), 'source timestamp')
            error = finite(observation.get('sync_error_ms'), 'capture sync error')
            if after < before or error < 0 or not is_present(observation.get('frame_id')):
                raise ValueError('invalid capture interval, sync error, or frame id')
            valid.append(dict(observation, capture_before_server_ms=before,
                              capture_after_server_ms=after,
                              source_timestamp_server_ms=source, sync_error_ms=error))
        except ValueError:
            errors.append('invalid_observation:%d' % index)
    return valid, errors


def load_session(artifact, artifact_path=None, producer_session=None):
    """Read a producer session only from an explicit sidecar or a shared PNG directory."""
    if producer_session is not None:
        return producer_session, 'provided'
    if isinstance(artifact.get('producer_session'), dict):  # Test/export convenience.
        return artifact['producer_session'], 'embedded'
    paths = []
    for observation in artifact.get('observations', []):
        if isinstance(observation, dict) and isinstance(observation.get('capture_artifact'), str):
            paths.append(Path(observation['capture_artifact']))
    parents = {path.parent for path in paths if path.is_absolute()}
    if len(parents) == 1:
        path = parents.pop() / 'session.json'
        if path.is_file():
            try:
                with path.open(encoding='utf-8') as file:
                    return json.load(file), str(path)
            except (OSError, json.JSONDecodeError):
                return None, str(path) + ':unreadable'
    return None, None


def session_status(session):
    if not isinstance(session, dict):
        return ['producer_session_missing_or_ambiguous'], {'status': 'not_reported'}
    reasons, values = [], {}
    try:
        attempts = int(session['capture_attempt_count'])
        emitted = int(session['jsonl_emitted_count'])
        ocr_failures = int(session['ocr_failure_count'])
        if min(attempts, emitted, ocr_failures) < 0 or emitted + ocr_failures != attempts:
            raise ValueError('counts do not reconcile')
        values.update(capture_attempt_count=attempts, jsonl_emitted_count=emitted,
                      ocr_failure_count=ocr_failures,
                      unobserved_capture_attempt_count=attempts-emitted)
    except (KeyError, TypeError, ValueError):
        reasons.append('producer_session_counts_ambiguous')
    if session.get('attempts_reconciled_without_omissions') is not True:
        reasons.append('producer_attempts_not_reconciled')
    if session.get('metadata_mode') != 'vision':
        reasons.append('producer_metadata_mode_not_vision')
    if values.get('ocr_failure_count', 0) != 0:
        reasons.append('producer_ocr_failure_makes_run_ineligible')
    values['status'] = 'reported' if not reasons else 'ambiguous'
    return reasons, values


def nearest_rank(values, quantile):
    values = sorted(values)
    return values[max(0, math.ceil(len(values) * quantile) - 1)]


def latency_quantiles(observations, source_error):
    samples = []
    for observation in observations:
        uncertainty = observation['sync_error_ms'] + source_error + .5
        lower = observation['capture_before_server_ms'] - observation['source_timestamp_server_ms'] - uncertainty
        upper = observation['capture_after_server_ms'] - observation['source_timestamp_server_ms'] + uncertainty
        samples.append((lower, (lower + upper) / 2, upper))
    result = {}
    for name, quantile in (('p50', .5), ('p95', .95), ('p99', .99)):
        lower, estimate, upper = (nearest_rank([sample[index] for sample in samples], quantile)
                                  for index in range(3))
        result[name] = {
            'estimate_ms': estimate,
            'lower_measurement_error_bound_ms': lower,
            'upper_measurement_error_bound_ms': upper,
            'error_bound_ms': max(estimate - lower, upper - estimate),
            'method': 'nearest_rank; bounds include capture/source clock uncertainty, capture interval, and 0.5ms source rounding',
        }
    return result


def first_seen_and_stalls(observations, fps):
    first = {}
    for observation in sorted(observations, key=lambda item: item['capture_after_server_ms']):
        first.setdefault(observation['frame_id'], observation['capture_after_server_ms'])
    times = sorted(first.values())
    observation_start = min(item['capture_before_server_ms'] for item in observations)
    observation_end = max(item['capture_after_server_ms'] for item in observations)
    elapsed = observation_end - observation_start
    # Count distinct frames over the entire observation period.  Do not use just
    # first-to-last first-seen time: that conceals a freeze at either boundary.
    observed_fps = (len(times) * 1000 / elapsed) if elapsed > 0 else None
    gaps = [right - left for left, right in zip([observation_start] + times,
                                                 times + [observation_end])]
    threshold = 3_000 / fps
    return {
        'first_seen_unique_frame_count': len(times),
        'observed_first_seen_fps': observed_fps,
        'observation_duration_ms': elapsed,
        'limitation': 'first-seen OCR/capture rate over the full observation period only; sampling, OCR failures, and repeated displayed frames can understate delivery FPS, and it does not measure encoder or display refresh FPS',
        'stall_threshold_ms': threshold,
        'stall_count': sum(gap > threshold for gap in gaps),
        'max_first_seen_gap_ms': max(gaps) if gaps else None,
        'initial_first_seen_gap_ms': gaps[0] if gaps else None,
        'terminal_first_seen_gap_ms': gaps[-1] if gaps else None,
    }


def available_field(artifact, metadata, *names):
    for name in names:
        if name in artifact:
            return artifact[name]
        if isinstance(metadata, dict) and name in metadata:
            return metadata[name]
    return None


def analyze_formal(artifact, artifact_path=None, producer_session=None):
    """Analyze an eligible collector artifact; incomplete data is never a verdict."""
    reasons = []
    if not isinstance(artifact, dict):
        raise ValueError('collector artifact must be a JSON object')
    if artifact.get('collection_error'):
        reasons.append('collector_input_validation_failed')
    observations = artifact.get('observations')
    if not isinstance(observations, list) or not observations:
        reasons.append('collector_observations_missing')
        observations = []
    valid, observation_reasons = valid_observations(observations)
    reasons.extend(observation_reasons)
    capture, capture_reasons = formal_clock_status(
        artifact.get('calibration_start'), artifact.get('calibration_end'), valid,
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
        source_calibrations.get('end', {}).get('report'), valid,
        'offset_interval_server_minus_source_ms', 'source')
    reasons.extend(source_reasons)
    if not isinstance(source_artifact, dict) or source_artifact.get('run_id') != artifact.get('run_id'):
        reasons.append('source_clock_run_id_missing_or_mismatched')
    metadata_reasons, formal_metadata = metadata_status(artifact.get('metadata'))
    reasons.extend(metadata_reasons)
    if len(valid) < 1000:
        reasons.append('valid_sample_count_less_than_1000')
    elapsed = None
    if valid:
        elapsed = (max(item['capture_after_server_ms'] for item in valid) -
                   min(item['capture_before_server_ms'] for item in valid))
        if elapsed < 60_000:
            reasons.append('observed_elapsed_wall_clock_less_than_60s')
    else:
        reasons.append('observed_elapsed_wall_clock_not_computable')
    session, session_source = load_session(artifact, artifact_path, producer_session)
    session_reasons, drop_fields = session_status(session)
    reasons.extend(session_reasons)
    # A formal producer session must cover the observations but OCR failures are reported,
    # not silently counted as valid latency samples.
    if drop_fields.get('jsonl_emitted_count') != len(valid):
        reasons.append('producer_emitted_count_does_not_match_valid_observations')
    reasons = list(dict.fromkeys(reasons))
    result = {
        'kind': 'formal_video_latency_analysis',
        'run_id': artifact.get('run_id'),
        'eligible_for_formal_analysis': not reasons,
        'status': 'computed' if not reasons else 'inconclusive',
        'rejection_reasons': reasons,
        'capture_host_clock': capture,
        'source_window_clock': source,
        'observed_elapsed_wall_clock_ms': elapsed,
        'valid_sample_count': len(valid),
        'required_minimums': {'warmup_ms': 30_000, 'observed_elapsed_wall_clock_ms': 60_000,
                              'valid_samples': 1000},
        'producer_session': {'source': session_source, **drop_fields},
        'drops': drop_fields,
        'resources': available_field(artifact, artifact.get('metadata'), 'resources', 'resource_usage') or {'status': 'not_reported'},
        'errors': {'collection_error': artifact.get('collection_error'),
                   'raw_validation_error_count': sum(1 for value in artifact.get('raw_observations', [])
                                                     if isinstance(value, dict) and value.get('validation_error')),
                   'producer_ocr_failure_count': drop_fields.get('ocr_failure_count')},
        'metric_verdict': 'not_computed' if reasons else 'computed_without_pass_fail_verdict',
    }
    if not reasons:
        source_error = finite(source_calibrations['start']['report']['sync_error_ms'], 'source sync error')
        source_error = max(source_error, finite(source_calibrations['end']['report']['sync_error_ms'], 'source sync error'))
        fps = finite(artifact['metadata']['video']['fps'], 'video.fps')
        if fps <= 0:
            # This is normally caught by condition validation only if a nonsensical value was recorded.
            result['status'] = 'inconclusive'
            result['eligible_for_formal_analysis'] = False
            result['rejection_reasons'].append('video_fps_not_positive')
            result['metric_verdict'] = 'not_computed'
        else:
            result['latency_quantiles_ms'] = latency_quantiles(valid, source_error)
            result['observed_first_seen'] = first_seen_and_stalls(valid, fps)
    return result


def condition_fingerprint(artifact):
    metadata = artifact.get('metadata')
    reasons, formal = metadata_status(metadata)
    if reasons:
        return None, reasons
    condition = {group: {field: metadata[group][field] for field in fields}
                 for group, fields in CONDITION_FIELDS.items()}
    condition['formal_measurement'] = {field: formal[field] for field in FORMAL_FIELDS
                                       if field != 'warmup_elapsed_ms'}
    return condition, []


def compare_ab_pairs(pairs):
    """Compare repeated A/B pairs only after every artifact independently qualifies."""
    results, reasons, deltas, fingerprint, run_ids, paths = [], [], [], None, set(), set()
    if len(pairs) < 3:
        reasons.append('at_least_three_repeated_ab_pairs_required')
    for pair_index, (a, b) in enumerate(pairs):
        a_result, b_result = analyze_formal(a[0], artifact_path=a[1], producer_session=a[2]), analyze_formal(b[0], artifact_path=b[1], producer_session=b[2])
        results.append({'pair_index': pair_index, 'a': a_result, 'b': b_result})
        pair_identities = []
        for role, entry in (('a', a), ('b', b)):
            run_id, path = entry[0].get('run_id'), entry[1]
            if not isinstance(run_id, str) or not run_id.strip():
                reasons.append('pair_%d_%s_artifact_identity_missing' % (pair_index, role))
                continue
            if run_id in run_ids:
                reasons.append('duplicate_artifact_identity:%s' % run_id)
            run_ids.add(run_id)
            if path and path in paths:
                reasons.append('duplicate_artifact_path:%s' % path)
            if path:
                paths.add(path)
            pair_identities.append(run_id)
        if len(pair_identities) == 2 and pair_identities[0] == pair_identities[1]:
            reasons.append('pair_%d_a_and_b_share_run_id' % pair_index)
        if not a_result['eligible_for_formal_analysis'] or not b_result['eligible_for_formal_analysis']:
            reasons.append('pair_%d_has_ineligible_run' % pair_index)
            continue
        for artifact in (a[0], b[0]):
            candidate, candidate_reasons = condition_fingerprint(artifact)
            if candidate_reasons:
                reasons.append('pair_%d_condition_metadata_incomplete' % pair_index)
            elif fingerprint is None:
                fingerprint = candidate
            elif candidate != fingerprint:
                reasons.append('pair_%d_conditions_do_not_match' % pair_index)
        if a_result['eligible_for_formal_analysis'] and b_result['eligible_for_formal_analysis']:
            deltas.append({name: {
                'estimate_ms': b_result['latency_quantiles_ms'][name]['estimate_ms'] -
                               a_result['latency_quantiles_ms'][name]['estimate_ms'],
                'lower_measurement_error_bound_ms':
                    b_result['latency_quantiles_ms'][name]['lower_measurement_error_bound_ms'] -
                    a_result['latency_quantiles_ms'][name]['upper_measurement_error_bound_ms'],
                'upper_measurement_error_bound_ms':
                    b_result['latency_quantiles_ms'][name]['upper_measurement_error_bound_ms'] -
                    a_result['latency_quantiles_ms'][name]['lower_measurement_error_bound_ms'],
            } for name in ('p50', 'p95', 'p99')})
    reasons = list(dict.fromkeys(reasons))
    delta_summary = None
    if not reasons:
        delta_summary = {}
        for name in ('p50', 'p95', 'p99'):
            estimates = [delta[name]['estimate_ms'] for delta in deltas]
            delta_summary[name] = {
                'per_pair': [delta[name] for delta in deltas],
                'mean_estimate_ms': sum(estimates) / len(estimates),
                'min_estimate_ms': min(estimates),
                'max_estimate_ms': max(estimates),
                'sample_standard_deviation_ms':
                    math.sqrt(sum((value - sum(estimates) / len(estimates)) ** 2 for value in estimates) /
                              (len(estimates) - 1)) if len(estimates) > 1 else None,
                'limitation': 'per-pair bounds are measurement-error bounds; pair-to-pair spread is reported, not converted into an automatic pass/fail confidence claim',
            }
    return {'kind': 'formal_video_latency_ab_pair_comparison',
            'status': 'not_computed' if reasons else 'computed_without_pass_fail_verdict',
            'comparison_reasons': reasons,
            'pair_count': len(pairs), 'condition_fingerprint': fingerprint,
            'pair_results': results,
            'b_minus_a_latency_delta_ms': delta_summary,
            'verdict': 'not_computed' if reasons else 'computed_without_pass_fail_verdict'}


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('csv', nargs='?')
    parser.add_argument('--collector-artifact')
    parser.add_argument('--producer-session', help='producer session.json for one collector artifact')
    parser.add_argument('--ab-pair', action='append', nargs=2, metavar=('A', 'B'),
                        help='repeat at least three times to compare formal A/B artifact pairs')
    args=parser.parse_args()
    modes = int(bool(args.csv)) + int(bool(args.collector_artifact)) + int(bool(args.ab_pair))
    if modes != 1 or (args.producer_session and not args.collector_artifact):
        parser.error('provide exactly one CSV input, --collector-artifact, or --ab-pair')
    if args.collector_artifact:
        with open(args.collector_artifact, encoding='utf-8') as artifact_file:
            artifact = json.load(artifact_file)
        session = None
        if args.producer_session:
            with open(args.producer_session, encoding='utf-8') as session_file:
                session = json.load(session_file)
        print(json.dumps(analyze_formal(artifact, args.collector_artifact, session), indent=2))
    elif args.ab_pair:
        pairs = []
        for a_path, b_path in args.ab_pair:
            with open(a_path, encoding='utf-8') as file:
                a = json.load(file)
            with open(b_path, encoding='utf-8') as file:
                b = json.load(file)
            pairs.append(((a, a_path, None), (b, b_path, None)))
        print(json.dumps(compare_ab_pairs(pairs), indent=2))
    else:
        with open(args.csv,newline='') as file:
            print(json.dumps(analyze(list(csv.DictReader(file))),indent=2))
