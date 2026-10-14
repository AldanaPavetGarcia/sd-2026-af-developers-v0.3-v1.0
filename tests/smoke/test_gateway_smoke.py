#!/usr/bin/env python3
"""Tests for tests/smoke/gateway_smoke.py, the gate of make blue-green-switch (US-305 AC3).

A gate that passes a broken slot switches traffic to it, so the suite is run against a fake
gateway that fails one way at a time and must reject each of them.
"""

import json
import os
import subprocess
import sys
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SUITE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gateway_smoke.py")


class FakeGateway(BaseHTTPRequestHandler):
    """A gateway that answers correctly unless FakeGateway.broken names something to break."""

    broken = None
    version = "0.3.2"

    def reply(self, status, body):
        payload = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("X-EOP-Version", FakeGateway.version)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):  # noqa: N802 (http.server naming)
        if self.path == "/v1/health":
            if FakeGateway.broken == "health":
                self.reply(503, {"data": {"status": "unavailable"}})
            else:
                self.reply(200, {"data": {"status": "ok"}})
        elif self.path == "/v1/nodes":
            self.reply(200, {"data": {"nodes": []}} if FakeGateway.broken == "nodes" else {"data": []})
        else:
            self.reply(404, {"error": {"code": "ROUTE_NOT_FOUND"}})

    def do_POST(self):  # noqa: N802 (http.server naming)
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if FakeGateway.broken == "analysis":
            self.reply(504, {"error": {"code": "ANALYSIS_TIMEOUT"}})
            return
        groups = [["vault-a", "vault-b"], ["vault-c"]]
        if FakeGateway.broken == "wrong-result":
            groups = [body["nodes"]]
        self.reply(200, {"data": {"result": {"algorithms": {"connected_components": groups}}}})

    def log_message(self, *_):
        pass


class GatewaySmokeTest(unittest.TestCase):
    def setUp(self):
        FakeGateway.broken, FakeGateway.version = None, "0.3.2"
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), FakeGateway)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()

    def run_suite(self, *args):
        return subprocess.run([sys.executable, SUITE, self.url, *args],
                              capture_output=True, text=True, timeout=60, check=False)

    def test_healthy_slot_passes(self):
        out = self.run_suite("--expect-version", "0.3.2")
        self.assertEqual(out.returncode, 0, out.stdout)

    def test_each_failure_blocks_the_switch(self):
        for broken in ("health", "nodes", "analysis", "wrong-result"):
            with self.subTest(broken=broken):
                FakeGateway.broken = broken
                out = self.run_suite()
                self.assertEqual(out.returncode, 1, out.stdout)
                self.assertIn("FAIL", out.stdout)

    def test_a_slot_serving_another_version_is_rejected(self):
        # Guards against smoke-testing the slot that is already live instead of the new one.
        FakeGateway.version = "0.3.1"
        out = self.run_suite("--expect-version", "0.3.2")
        self.assertEqual(out.returncode, 1, out.stdout)
        self.assertIn("X-EOP-Version: 0.3.1", out.stdout)

    def test_an_unreachable_slot_is_rejected(self):
        self.server.shutdown()
        self.server.server_close()
        out = self.run_suite()
        self.assertEqual(out.returncode, 1, out.stdout)


if __name__ == "__main__":
    unittest.main()
