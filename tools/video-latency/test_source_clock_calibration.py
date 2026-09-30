#!/usr/bin/env python3
"""Protocol and formal-eligibility regression tests for source clock calibration."""
import importlib.util
import json
import copy
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock
from urllib.error import HTTPError
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parent
FORMAL_MANIFEST = ROOT.parents[1] / 'docs/measurements/2026-09-29-video-latency-formal-manifest.json'
ANALYZE_SPEC = importlib.util.spec_from_file_location('video_latency_analyze', ROOT / 'analyze.py')
ANALYZE = importlib.util.module_from_spec(ANALYZE_SPEC)
ANALYZE_SPEC.loader.exec_module(ANALYZE)


def request(url, method='GET', value=None):
    data = None if value is None else json.dumps(value).encode()
    headers = {'Content-Type': 'application/json'} if data else {}
    with urlopen(Request(url, data=data, method=method, headers=headers), timeout=2) as response:
        return json.loads(response.read())


def source_report(run_id, phase):
    boundary = 100.0 if phase == 'start' else 170.0
    return {
        'run_id': run_id, 'phase': phase,
        'calibration_started_server_ms': boundary - 1,
        'calibration_completed_server_ms': boundary,
        'selected_sample_index': 0,
        'offset_server_minus_source_ms': 10.0,
        'sync_error_ms': 1.0,
        'offset_interval_server_minus_source_ms': [9.0, 11.0],
        'samples': [{'sample_index': 0, 'local_after_ms': 91.0,
                     'request': {'url': '/1/clock?n=0', 'local_before_ms': 89.0},
                     'response': {'status': 200, 'body': '{"received":99,"sent":100}'}}],
    }


class SourceClockCalibrationTest(unittest.TestCase):
    def setUp(self):
        self.server = subprocess.Popen(
            [sys.executable, str(ROOT / 'server.py'), '--port', '0'],
            stdout=subprocess.PIPE, text=True)
        self.url = json.loads(self.server.stdout.readline())['url']

    def tearDown(self):
        self.server.terminate()
        self.server.wait(timeout=5)
        self.server.stdout.close()

    def test_server_preserves_raw_reports_and_refuses_duplicate_phase(self):
        run_id = 'protocol-fixture'
        request(self.url + '/runs/' + run_id + '/start', 'POST', {})
        request(self.url + '/runs/' + run_id + '/calibrations', 'POST',
                source_report(run_id, 'start'))
        with self.assertRaises(HTTPError):
            request(self.url + '/runs/' + run_id + '/calibrations', 'POST',
                    source_report(run_id, 'start'))
        artifact = request(self.url + '/runs/' + run_id)
        raw = artifact['source_calibrations']['start']['report']['samples'][0]
        self.assertEqual(raw['request']['url'], '/1/clock?n=0')
        self.assertEqual(raw['response']['body'], '{"received":99,"sent":100}')

    def test_formal_manifest_template_matches_collector_metadata_contract(self):
        template = json.loads(FORMAL_MANIFEST.read_text())
        for group, fields in ANALYZE.CONDITION_FIELDS.items():
            self.assertIsInstance(template.get(group), dict)
            self.assertTrue(set(fields).issubset(template[group]))
        self.assertIsInstance(template.get('formal_measurement'), dict)
        self.assertTrue(set(ANALYZE.FORMAL_FIELDS).issubset(template['formal_measurement']))

    def test_invalidation_retains_start_report_and_prevents_end_report(self):
        run_id = 'invalidated-fixture'
        request(self.url + '/runs/' + run_id + '/start', 'POST', {})
        request(self.url + '/runs/' + run_id + '/calibrations', 'POST',
                source_report(run_id, 'start'))
        artifact = request(self.url + '/runs/' + run_id + '/invalidate', 'POST',
                           {'reason': 'source_tab_hidden_during_formal_run'})
        self.assertEqual(artifact['status'], 'invalidated')
        self.assertIn('start', artifact['source_calibrations'])
        with self.assertRaises(HTTPError):
            request(self.url + '/runs/' + run_id + '/calibrations', 'POST',
                    source_report(run_id, 'end'))

    def test_synthetic_clock_fixture_is_preliminary_and_rejects_missing_calibration(self):
        fixture = json.loads((ROOT / 'testdata/source-clock-calibration-valid.json').read_text())
        result = ANALYZE.analyze_formal(fixture)
        self.assertFalse(result['eligible_for_formal_analysis'])
        self.assertIn('valid_sample_count_less_than_1000', result['rejection_reasons'])
        self.assertIn('observed_elapsed_wall_clock_not_computable', result['rejection_reasons'])
        del fixture['source_clock_calibration']['artifact']['source_calibrations']['end']
        result = ANALYZE.analyze_formal(fixture)
        self.assertFalse(result['eligible_for_formal_analysis'])
        self.assertIn('source_calibration_missing', result['rejection_reasons'])

    def test_eligible_collector_artifact_computes_bounds_fps_and_quality_fields(self):
        run_id = 'formal-fixture'
        def calibration(interval_name, start, end, offset=10.0, error=1.0):
            return {interval_name: [offset-error, offset+error],
                    'offset_server_minus_' + ('source_ms' if 'source' in interval_name else 'local_ms'): offset,
                    'sync_error_ms': error,
                    'calibration_started_server_ms': start,
                    'calibration_completed_server_ms': end,
                    'selected_sample_index': 0,
                    'samples': [{'sample_index': 0, 'local_after_ms': end-offset,
                                 'request': {'local_before_ms': start-offset},
                                 'response': {'body': '{}'}}]}
        observations = []
        for index in range(1000):
            before = 1_000 + index * 61
            observations.append({'frame_id': str(index),
                                 'source_timestamp_server_ms': before - 100,
                                 'capture_before_server_ms': before,
                                 'capture_after_server_ms': before + 1,
                                 'sync_error_ms': 1,
                                 'capture_artifact': '/tmp/formal/frame-%04d.png' % index})
        fixture = {
            'run_id': run_id, 'collection_error': None, 'observations': observations,
            'calibration_start': calibration('offset_interval_server_minus_local_ms', 0, 900),
            'calibration_end': calibration('offset_interval_server_minus_local_ms', 62_100, 63_000),
            'source_clock_calibration': {'artifact': {'run_id': run_id,
                'source_calibrations': {
                    'start': {'report': calibration('offset_interval_server_minus_source_ms', 0, 900)},
                    'end': {'report': calibration('offset_interval_server_minus_source_ms', 62_100, 63_000)}}}},
            'metadata': {
                'source': {'resolution': '1920x1080', 'refresh_hz': 60, 'device': 'Windows', 'browser': 'Edge'},
                'video': {'codec': 'AVC420', 'bitrate_kbps': 8000, 'gop': 30, 'fps': 30},
                'client': {'application': 'Windows App', 'version': '1', 'platform': 'macOS', 'window': 'id:1', 'capture_api': 'ScreenCaptureKit'},
                'network': {'path': 'lan', 'transport': 'TCP', 'rtt_ms': 2, 'loss_percent': 0, 'contention': False},
                'formal_measurement': {'warmup_elapsed_ms': 30_000, 'condition_id': 'same-quality',
                                       'pattern_version': '1', 'workload': 'timestamp',
                                       'capture_timestamp_semantics_verified': True,
                                       'source_geometry': '1920x1080@60',
                                       'receiver_geometry': '1920x1080@60',
                                       'capture_geometry': '1920x1080',
                                       'capture_settings': {'shows_cursor': False, 'interval_ms': 61},
                                       'video_quality_verified': True, 'actual_gop': 30},
                'resources': {'cpu_percent': 10},
            },
            'producer_session': {'capture_attempt_count': 1000, 'jsonl_emitted_count': 1000,
                                 'ocr_failure_count': 0, 'attempts_reconciled_without_omissions': True,
                                 'metadata_mode': 'vision'},
        }
        result = ANALYZE.analyze_formal(fixture)
        self.assertTrue(result['eligible_for_formal_analysis'], result)
        self.assertEqual(result['status'], 'computed')
        self.assertEqual(result['latency_quantiles_ms']['p99']['estimate_ms'], 100.5)
        self.assertGreater(result['latency_quantiles_ms']['p99']['error_bound_ms'], 0)
        self.assertAlmostEqual(result['observed_first_seen']['observed_first_seen_fps'], 1000 / (999 * 61 + 1) * 1000)
        self.assertEqual(result['drops']['unobserved_capture_attempt_count'], 0)
        self.assertEqual(result['resources']['cpu_percent'], 10)
        # Exercise the real collector-artifact CLI path with independently saved
        # artifact/session files; this remains a synthetic fixture, never acceptance evidence.
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            artifact_path, session_path = directory / 'collector.json', directory / 'session.json'
            cli_fixture = copy.deepcopy(fixture)
            session = cli_fixture.pop('producer_session')
            artifact_path.write_text(json.dumps(cli_fixture))
            session_path.write_text(json.dumps(session))
            completed = subprocess.run(
                [sys.executable, str(ROOT / 'analyze.py'), '--collector-artifact', str(artifact_path),
                 '--producer-session', str(session_path)], text=True, capture_output=True, timeout=10)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(json.loads(completed.stdout)['status'], 'computed')
            evidence_dir = os.environ.get('VIDEO_LATENCY_EVIDENCE_DIR')
            if evidence_dir:
                evidence = Path(evidence_dir)
                evidence.mkdir(parents=True, exist_ok=True)
                (evidence / 'synthetic-collector-artifact.json').write_text(json.dumps(cli_fixture, indent=2))
                (evidence / 'synthetic-producer-session.json').write_text(json.dumps(session, indent=2))
                (evidence / 'synthetic-formal-analysis.json').write_text(completed.stdout)
        fixture['metadata']['network']['contention'] = False
        self.assertTrue(ANALYZE.analyze_formal(fixture)['eligible_for_formal_analysis'])
        fixture['observations'] = [dict(item, frame_id='frozen') for item in observations]
        frozen = ANALYZE.analyze_formal(fixture)
        self.assertTrue(frozen['eligible_for_formal_analysis'])
        self.assertGreater(frozen['observed_first_seen']['terminal_observation_gap_ms'], 60_000)
        self.assertEqual(frozen['observed_first_seen']['observation_gap_count'], 1)
        self.assertEqual(frozen['observed_first_seen']['video_stall_status'],
                         'not_measurable_from_sampled_captures')
        fixture['producer_session'] = dict(fixture['producer_session'],
                                           capture_attempt_count=1001, ocr_failure_count=1)
        failed_ocr = ANALYZE.analyze_formal(fixture)
        self.assertFalse(failed_ocr['eligible_for_formal_analysis'])
        self.assertIn('producer_ocr_failure_makes_run_ineligible', failed_ocr['rejection_reasons'])

    def test_ab_comparison_requires_three_complete_matching_pairs(self):
        base = {'metadata': {
            'source': {'resolution': '1920x1080', 'refresh_hz': 60, 'device': 'Windows', 'browser': 'Edge'},
            'video': {'codec': 'AVC420', 'bitrate_kbps': 8000, 'gop': 30, 'fps': 30},
            'client': {'application': 'Windows App', 'version': '1', 'platform': 'macOS', 'window': 'id:1', 'capture_api': 'ScreenCaptureKit'},
            'network': {'path': 'lan', 'transport': 'TCP', 'rtt_ms': 2, 'loss_percent': 0, 'contention': False},
        }}
        base['metadata']['formal_measurement'] = {
            'warmup_elapsed_ms': 30_000, 'condition_id': 'same', 'pattern_version': '1',
            'workload': 'timestamp', 'capture_timestamp_semantics_verified': True,
            'source_geometry': '1920x1080@60', 'receiver_geometry': '1920x1080@60',
            'capture_geometry': '1920x1080', 'capture_settings': {'interval_ms': 61},
            'video_quality_verified': True, 'actual_gop': 30}
        self.assertEqual(ANALYZE.compare_ab_pairs([])['status'], 'not_computed')
        self.assertIn('at_least_three_repeated_ab_pairs_required',
                      ANALYZE.compare_ab_pairs([])['comparison_reasons'])
        def computed(run_id, estimate):
            return {'run_id': run_id, 'metadata': base['metadata'], 'producer_session': {}}
        def fake_analysis(artifact, **_):
            values = {name: {'estimate_ms': artifact['estimate'],
                             'lower_measurement_error_bound_ms': artifact['estimate'] - 1,
                             'upper_measurement_error_bound_ms': artifact['estimate'] + 1}
                      for name in ('p50', 'p95', 'p99')}
            return {'eligible_for_formal_analysis': True, 'latency_quantiles_ms': values}
        pairs = []
        for index in range(3):
            a, b = computed('a-%d' % index, 100), computed('b-%d' % index, 90)
            a['estimate'], b['estimate'] = 100, 90
            pairs.append(((a, 'a-%d.json' % index, None), (b, 'b-%d.json' % index, None)))
        with mock.patch.object(ANALYZE, 'analyze_formal', side_effect=fake_analysis):
            comparison = ANALYZE.compare_ab_pairs(pairs)
        self.assertEqual(comparison['status'], 'computed_without_pass_fail_verdict')
        self.assertEqual(comparison['b_minus_a_latency_delta_ms']['p99']['mean_estimate_ms'], -10)
        self.assertEqual(comparison['b_minus_a_latency_delta_ms']['p99']['per_pair'][0]
                         ['upper_measurement_error_bound_ms'], -8)
        fixture = json.loads((ROOT / 'testdata/source-clock-calibration-valid.json').read_text())
        del fixture['calibration_end']
        result = ANALYZE.analyze_formal(fixture)
        self.assertFalse(result['eligible_for_formal_analysis'])
        self.assertIn('capture_host_calibration_missing', result['rejection_reasons'])
        fixture = json.loads((ROOT / 'testdata/source-clock-calibration-valid.json').read_text())
        fixture['source_clock_calibration']['artifact']['source_calibrations'] = []
        result = ANALYZE.analyze_formal(fixture)
        self.assertFalse(result['eligible_for_formal_analysis'])
        self.assertIn('source_calibration_missing', result['rejection_reasons'])

    def test_malformed_jsonl_writes_append_only_failure_artifact(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            metadata = directory / 'metadata.json'
            output = directory / 'failed.json'
            metadata.write_text('{}')
            command = [sys.executable, str(ROOT / 'collector.py'), '--server-url', self.url,
                       '--metadata', str(metadata), '--run-id', 'malformed-fixture',
                       '--output', str(output), '--calibration-samples', '1',
                       '--source-clock-wait', '0']
            completed = subprocess.run(command, input='{bad json}\n{"frame_id":1}\n',
                                       text=True, capture_output=True, timeout=10)
            self.assertEqual(completed.returncode, 2)
            artifact = json.loads(output.read_text())
            self.assertEqual(artifact['status'], 'collection_failed')
            self.assertEqual(len(artifact['raw_observations']), 2)
            self.assertEqual(artifact['raw_observations'][0]['jsonl'], '{bad json}')
            self.assertIn('validation_error', artifact['raw_observations'][1])

    def test_collector_records_bracketed_source_and_capture_calibrations(self):
        run_id = 'end-to-end-fixture'
        errors = []
        source_started = threading.Event()

        def source_page_simulator():
            try:
                while True:
                    try:
                        run = request(self.url + '/runs/' + run_id)
                    except HTTPError as error:
                        if error.code == 404:
                            time.sleep(.01)
                            continue
                        raise
                    if run['status'] == 'start_requested':
                        start = source_report(run_id, 'start')
                        # Synthetic fixture boundaries deliberately cover the child
                        # process's monotonic epoch without claiming live timing.
                        start['calibration_completed_server_ms'] = 0.0
                        request(self.url + '/runs/' + run_id + '/calibrations', 'POST', start)
                        source_started.set()
                    elif run['status'] == 'end_requested':
                        end = source_report(run_id, 'end')
                        end['calibration_started_server_ms'] = 1e20
                        request(self.url + '/runs/' + run_id + '/calibrations', 'POST', end)
                        return
                    time.sleep(.01)
            except Exception as error:  # Propagate thread failures into the test.
                errors.append(error)

        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            metadata, output = directory / 'metadata.json', directory / 'collector.json'
            metadata.write_text('{}')
            thread = threading.Thread(target=source_page_simulator, daemon=True)
            thread.start()
            command = [sys.executable, str(ROOT / 'collector.py'), '--server-url', self.url,
                       '--metadata', str(metadata), '--run-id', run_id, '--output', str(output),
                       '--calibration-samples', '1', '--source-clock-wait', '2']
            collector = subprocess.Popen(command, stdin=subprocess.PIPE, text=True,
                                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertTrue(source_started.wait(timeout=3))
            time.sleep(.05)
            now = time.monotonic_ns()
            collector.stdin.write(json.dumps({
                'capture_before_monotonic_ns': now,
                'capture_after_monotonic_ns': now + 1_000_000,
                'source_timestamp_server_ms': 1,
                'frame_id': 'fixture', 'capture_artifact': 'fixture.png'}) + '\n')
            collector.stdin.close()
            self.assertEqual(collector.wait(timeout=10), 0, collector.stderr.read())
            collector.stdout.close()
            collector.stderr.close()
            thread.join(timeout=2)
            self.assertFalse(errors)
            artifact = json.loads(output.read_text())
            self.assertEqual(artifact['source_clock_calibration']['status']['bracketing_status'],
                             'bracketed', json.dumps({
                                 'status': artifact['source_clock_calibration']['status'],
                                 'observation': artifact['observations'][0],
                                 'source': artifact['source_clock_calibration']['artifact']}, indent=2))
            self.assertIn('samples', artifact['calibration_start'])
            self.assertIn('samples', artifact['source_clock_calibration']['artifact']
                          ['source_calibrations']['start']['report'])

    def test_pattern_serializes_run_operations_and_invalidates_restart_paths(self):
        page = (ROOT / 'pattern.html').read_text()
        self.assertIn('runOperation', page)
        self.assertIn('finally { runOperation=false; }', page)
        self.assertIn('manual_resync_during_formal_run', page)
        self.assertIn('source_tab_hidden_during_formal_run', page)


if __name__ == '__main__':
    unittest.main()
