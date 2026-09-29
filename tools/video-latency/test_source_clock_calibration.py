#!/usr/bin/env python3
"""Protocol and formal-eligibility regression tests for source clock calibration."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parent
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

    def test_formal_analysis_rejects_missing_end_and_unbracketed_source(self):
        fixture = json.loads((ROOT / 'testdata/source-clock-calibration-valid.json').read_text())
        self.assertTrue(ANALYZE.analyze_formal(fixture)['eligible_for_formal_analysis'])
        del fixture['source_clock_calibration']['artifact']['source_calibrations']['end']
        result = ANALYZE.analyze_formal(fixture)
        self.assertFalse(result['eligible_for_formal_analysis'])
        self.assertIn('source_calibration_missing', result['rejection_reasons'])
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
