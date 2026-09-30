#!/usr/bin/env python3
"""Protocol tests for the formal video-latency server."""
import json
from pathlib import Path
import subprocess
import sys
import unittest
from urllib.error import HTTPError
from urllib.request import HTTPRedirectHandler, Request, build_opener


SERVER = Path(__file__).with_name('server.py')


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, request, response, code, msg, headers, newurl):
        return None


class ServerProtocolTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.process = subprocess.Popen(
            [sys.executable, str(SERVER), '--bind', '127.0.0.1', '--port', '0'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        line = cls.process.stdout.readline()
        if not line:
            stderr = cls.process.stderr.read()
            raise RuntimeError('server did not start: %s' % stderr)
        cls.server_info = json.loads(line)
        cls.base_url = cls.server_info['url'].rsplit('/1', 1)[0]
        cls.opener = build_opener(NoRedirect)

    @classmethod
    def tearDownClass(cls):
        cls.process.terminate()
        try:
            cls.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            cls.process.kill()
            cls.process.wait()
        cls.process.stdout.close()
        cls.process.stderr.close()

    def request(self, path, method='GET', body=None):
        request = Request(self.base_url + path, data=body, method=method)
        try:
            return self.opener.open(request)
        except HTTPError as error:
            return error

    def test_short_alias_redirects_to_query_source(self):
        response = self.request('/1/d1')
        self.assertEqual(response.status, 302)
        self.assertEqual(response.headers['Location'], '/1?run_id=d1')
        self.assertEqual(response.headers['Cache-Control'], 'no-store')

    def test_invalid_or_nested_alias_is_not_redirected(self):
        self.assertEqual(self.request('/1/not%20a%20run').status, 404)
        self.assertEqual(self.request('/1/d1/extra').status, 404)

    def test_existing_source_clock_and_run_api_are_preserved(self):
        clock = self.request('/1/clock')
        self.assertEqual(clock.status, 200)
        payload = json.loads(clock.read())
        self.assertIn('received', payload)
        self.assertIn('sent', payload)

        source = self.request('/1?run_id=d1')
        self.assertEqual(source.status, 200)
        self.assertGreater(int(source.headers['Content-Length']), 1000)

        started = self.request('/1/runs/d1/start', method='POST', body=b'')
        self.assertEqual(started.status, 201)
        state = json.loads(started.read())
        self.assertEqual(state['run_id'], 'd1')
        self.assertEqual(state['status'], 'start_requested')


if __name__ == '__main__':
    unittest.main()
