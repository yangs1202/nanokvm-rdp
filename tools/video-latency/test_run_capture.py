#!/usr/bin/env python3
"""Prevent incompatible collector clocks before any run is registered."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import run_capture


class CollectorPythonPreflightTest(unittest.TestCase):
    def probe(self, platform, version):
        return subprocess.CompletedProcess([], 0, json.dumps({
            'platform': platform, 'version': version, 'executable': '/test/python',
        }))

    def arguments(self, root):
        return ['run_capture.py', '--run-id', 'test', '--run-dir', str(root / 'run'),
                '--server-url', 'http://127.0.0.1:1/1',
                '--metadata', str(root / 'source.json'), '--producer', '/no/producer',
                '--collector', '/no/collector', '--window-id', '1',
                '--python', sys.executable]

    def test_old_macos_rejected_before_artifacts_or_processes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(sys, 'argv', self.arguments(root)), \
                    patch.object(run_capture.subprocess, 'run',
                                 return_value=self.probe('darwin', [3, 9, 6])), \
                    patch.object(run_capture.subprocess, 'Popen') as start, \
                    patch.object(run_capture, 'read_run_state') as read:
                with self.assertRaisesRegex(SystemExit, 'must be >=3.10 on macOS'):
                    run_capture.main()
                start.assert_not_called()
                read.assert_not_called()
                self.assertFalse((root / 'run').exists())

    def test_supported_versions_and_non_macos(self):
        for platform, version in [('darwin', [3, 10, 0]), ('darwin', [3, 14, 6]),
                                  ('linux', [3, 9, 6])]:
            with self.subTest(platform=platform, version=version), \
                    patch.object(run_capture.subprocess, 'run',
                                 return_value=self.probe(platform, version)):
                result = run_capture.preflight_collector_python(sys.executable)
                self.assertTrue(Path(result['invoked_executable']).is_absolute())

    def test_unusable_interpreter_fails_closed(self):
        with patch.object(run_capture.subprocess, 'run',
                          side_effect=subprocess.TimeoutExpired('python', 10)):
            with self.assertRaisesRegex(SystemExit, 'preflight failed'):
                run_capture.preflight_collector_python(sys.executable)

    def test_metadata_records_exact_collector_command_before_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'source.json').write_text('{"preserved": true}')
            with patch.object(sys, 'argv', self.arguments(root)), \
                    patch.object(run_capture.subprocess, 'run',
                                 return_value=self.probe('darwin', [3, 14, 6])), \
                    patch.object(run_capture.subprocess, 'Popen',
                                 side_effect=OSError('test: no collector launched')) as start, \
                    contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(run_capture.main(), 1)
            metadata = json.loads((root / 'run' / 'metadata.json').read_text())
            self.assertTrue(metadata['preserved'])
            execution = metadata['capture_execution']
            self.assertEqual(execution['collector_command'], start.call_args.args[0])
            self.assertTrue(Path(execution['collector_command'][0]).is_absolute())
            self.assertTrue(Path(execution['wrapper_command'][0]).is_absolute())
            self.assertEqual(execution['collector_python']['version'], [3, 14, 6])


if __name__ == '__main__':
    unittest.main()
