#!/usr/bin/env python3
"""Synthetic payload/loopback HTTP tests; never launches a browser or tracker."""

import copy
import http.client
import json
import os
from pathlib import Path
import socket
import stat
import subprocess
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

from intake import AutomationSession, IntakeError, MAX_REPORT_BYTES, validate_payload
from serve import create_server


ROOT = Path(__file__).resolve().parent


def encode(value):
    return json.dumps(value, separators=(",", ":")).encode("utf-8")


class PayloadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # The current JS producer is authoritative; this also detects schema drift.
        source = """
import {createReport, unavailableContext, unsupported, ok} from './report.mjs';
const context = unavailableContext(unsupported('synthetic-unavailable'));
context.status = 'ok'; delete context.reason;
context.signals['navigator.language'] = ok('en-US');
console.log(JSON.stringify(createReport(context, context, {
  capturedAt:'2026-09-07T00:00:00.000Z', rendering:false,
  labels:{identity:'direct',visit:'1',build:'runtime-smoke'},
})));
"""
        result = subprocess.run(["node", "--input-type=module", "-e", source], cwd=ROOT,
                                check=True, capture_output=True, timeout=5)
        cls.example = json.loads(result.stdout)

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="mux-privacy-unit-")
        self.session = AutomationSession("direct", False, "runtime-smoke", 60, self.directory.name)
        self.report = copy.deepcopy(self.example)

    def tearDown(self):
        self.directory.cleanup()

    def reject(self, value, status=400):
        with self.assertRaises(IntakeError) as caught:
            validate_payload(value if isinstance(value, bytes) else encode(value), self.session.configuration)
        self.assertEqual(caught.exception.status, status)

    def test_js_producer_matches_server_schema(self):
        self.assertEqual(validate_payload(encode(self.report), self.session.configuration), self.report)

    def test_configuration_and_labels_are_bound_to_session(self):
        self.report["metadata"]["labels"]["identity"] = "tor"
        self.reject(self.report)
        self.report = copy.deepcopy(self.example)
        self.report["configuration"]["rendering"] = True
        self.reject(self.report)

    def test_unknown_schema_missing_signals_and_extra_data_rejected(self):
        for mutate in (
            lambda value: value.update(schemaVersion=99),
            lambda value: value["measurements"]["worker"]["signals"].pop("navigator.language"),
            lambda value: value.update(outputPath="/tmp/not-authorized"),
        ):
            value = copy.deepcopy(self.example)
            mutate(value)
            self.reject(value)

    def test_duplicate_keys_nonfinite_unicode_and_depth_rejected(self):
        for body in (b'{"schema":1,"schema":2}', b'{"value":NaN}', b'\xff', b'[' * 40 + b'0' + b']' * 40):
            self.reject(body)

    def test_payload_bounds(self):
        self.reject(b"", 413)
        self.reject(b" " * (MAX_REPORT_BYTES + 1), 413)

    def test_invalid_diagnostic_and_forged_comparison_rejected(self):
        self.report["measurements"]["window"]["signals"]["canvas.api"] = {
            "status": "unsupported", "reason": "synthetic", "value": "pretend",
        }
        self.reject(self.report)
        self.report = copy.deepcopy(self.example)
        self.report["windowWorkerComparison"][0]["outcome"] = "different"
        self.reject(self.report)

    def test_tokens_are_random_exact_and_expiring(self):
        other = AutomationSession("direct", False, "runtime-smoke", 60, self.directory.name)
        self.assertNotEqual(self.session.token, other.token)
        self.assertEqual(len(self.session.token), 64)
        self.session.authorize(self.session.token)
        for token in (None, "", "f" * 63, self.session.token + "x", "\u03a9" * 64):
            with self.assertRaises(IntakeError):
                self.session.authorize(token)
        self.session.expires_at = time.monotonic() - 1
        with self.assertRaises(IntakeError) as caught:
            self.session.authorize(self.session.token)
        self.assertEqual(caught.exception.status, 410)

    def test_atomic_publish_private_permissions_and_one_shot(self):
        original_replace = os.replace
        stages = []

        def publish(source, destination):
            self.assertFalse(self.session.report_path.exists())
            self.assertEqual(Path(destination), self.session.report_path)
            self.assertEqual(json.loads(Path(source).read_bytes()), self.report)
            self.assertEqual(stat.S_IMODE(Path(source).stat().st_mode), 0o600)
            stages.append(True)
            return original_replace(source, destination)

        with patch("intake.os.replace", side_effect=publish):
            self.assertEqual(self.session.accept(encode(self.report)), {"state": "stored", "reportsStored": 1})
        self.assertEqual(stages, [True])
        self.assertEqual(stat.S_IMODE(self.session.directory.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE(self.session.report_path.stat().st_mode), 0o600)
        self.assertEqual(json.loads(self.session.report_path.read_bytes()), self.report)
        with self.assertRaises(IntakeError) as caught:
            self.session.accept(encode(self.report))
        self.assertEqual(caught.exception.status, 409)

    def test_failed_publish_leaves_no_partial_report(self):
        with patch("intake.os.replace", side_effect=OSError("synthetic failure")):
            with self.assertRaises(OSError):
                self.session.accept(encode(self.report))
        self.assertFalse(self.session.report_path.exists())
        self.assertEqual(list(self.session.directory.iterdir()), [])
        self.assertEqual(self.session.status(), {"state": "waiting", "reportsStored": 0})


class HTTPTests(PayloadTests):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="mux-privacy-http-unit-")
        self.server = create_server(automation=True, label="direct", output_root=self.directory.name, lifetime=60)
        self.session = self.server.automation
        self.report = copy.deepcopy(self.example)
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
        self.thread.start()
        self.origin = f"http://127.0.0.1:{self.server.server_port}"
        self.base = f"/automation/{self.session.token}"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.directory.cleanup()

    def request(self, method, path, body=None, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=2)
        try:
            connection.request(method, path, body=body, headers=headers or {})
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def headers(self):
        return {"Origin": self.origin, "X-Mux-Privacy-Token": self.session.token, "Content-Type": "application/json"}

    def test_default_mode_has_no_intake_or_output_directory(self):
        with tempfile.TemporaryDirectory(prefix="mux-privacy-default-unit-") as root:
            with patch("intake.tempfile.mkdtemp", side_effect=AssertionError("Default mode must not create output")):
                server = create_server()
            thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
            thread.start()
            try:
                connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=2)
                connection.request("POST", "/automation/" + "0" * 64 + "/report", body=b"{}")
                response = connection.getresponse()
                self.assertEqual(response.status, 405)
                response.read()
                connection.close()
                self.assertIsNone(server.automation)
                self.assertEqual(list(Path(root).iterdir()), [])
            finally:
                server.shutdown(); server.server_close(); thread.join(timeout=2)
        with self.assertRaises(ValueError):
            create_server(label="direct")

    def test_automation_requires_explicit_label(self):
        with self.assertRaises(ValueError):
            create_server(automation=True)

    def test_capability_configuration_and_distinct_csp(self):
        code, headers, body = self.request("GET", self.base + "/configuration")
        self.assertEqual(code, 200)
        self.assertEqual(json.loads(body), self.session.configuration)
        self.assertNotIn("Access-Control-Allow-Origin", headers)
        self.assertIn("no-store", headers["Cache-Control"])
        code, headers, _body = self.request("GET", "/index.html")
        self.assertEqual(code, 200)
        self.assertIn("connect-src 'none'", headers["Content-Security-Policy"])
        code, headers, body = self.request("GET", self.base + "/index.html")
        self.assertEqual(code, 200)
        self.assertIn("connect-src 'self'", headers["Content-Security-Policy"])
        self.assertIn(b"explicit local automation", body)

    def test_wrong_tokens_origins_and_hosts_rejected(self):
        code, _headers, _body = self.request("GET", "/automation/" + "0" * 64 + "/status")
        self.assertEqual(code, 403)
        for key, value in (("Origin", "http://example.invalid"), ("X-Mux-Privacy-Token", "0" * 64), ("Host", "example.invalid")):
            headers = self.headers(); headers[key] = value
            code, _headers, _body = self.request("POST", self.base + "/report", encode(self.report), headers)
            self.assertEqual(code, 403)
        self.assertFalse(self.session.report_path.exists())

    def test_traversal_and_unlisted_assets_rejected(self):
        for path in (self.base + "/../report.json", self.base + "/%2e%2e/report.json", self.base + "/report.json", self.base + "/status?path=/tmp/x", "/intake.py", "/automation.html"):
            code, _headers, _body = self.request("GET", path)
            self.assertEqual(code, 404)

    def test_http_payload_limit_transfer_and_content_type(self):
        for extra, expected in (({"Content-Length": str(MAX_REPORT_BYTES + 1)}, 413), ({"Transfer-Encoding": "chunked"}, 400), ({"Content-Type": "text/plain"}, 415), ({"Content-Encoding": "gzip"}, 415)):
            headers = self.headers(); headers.update(extra)
            code, _headers, _body = self.request("POST", self.base + "/report", b"{}", headers)
            self.assertEqual(code, expected)
        self.assertFalse(self.session.report_path.exists())

    def test_http_schema_rejection_then_atomic_capture_and_replay_rejection(self):
        code, _headers, body = self.request("POST", self.base + "/report", b'{"secret":"do-not-log"}', self.headers())
        self.assertEqual(code, 400)
        self.assertNotIn(b"do-not-log", body)
        self.assertFalse(self.session.report_path.exists())
        code, _headers, body = self.request("POST", self.base + "/report", encode(self.report), self.headers())
        self.assertEqual(code, 201)
        self.assertEqual(json.loads(body), {"state": "stored", "reportsStored": 1})
        code, _headers, body = self.request("GET", self.base + "/status")
        self.assertEqual(code, 200)
        self.assertEqual(json.loads(body), {"state": "stored", "reportsStored": 1})
        code, _headers, _body = self.request("POST", self.base + "/report", encode(self.report), self.headers())
        self.assertEqual(code, 409)
        self.assertEqual(json.loads(self.session.report_path.read_bytes()), self.report)

    def test_connection_has_absolute_deadline(self):
        with patch("serve.CONNECTION_DEADLINE_SECONDS", 0.05):
            connection = socket.create_connection(("127.0.0.1", self.server.server_port), timeout=1)
            try:
                connection.sendall(b"GET /index.html HTTP/1.1\r\n")
                self.assertEqual(connection.recv(1), b"")
            finally:
                connection.close()

    def test_unsupported_methods_never_store(self):
        for method in ("PUT", "PATCH", "DELETE", "OPTIONS"):
            code, _headers, _body = self.request(method, self.base + "/report", b"{}", self.headers())
            self.assertEqual(code, 405)
        self.assertFalse(self.session.report_path.exists())


if __name__ == "__main__":
    unittest.main()
