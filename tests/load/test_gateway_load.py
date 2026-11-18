#!/usr/bin/env python3
"""Tests for tests/load/gateway_load.py against a local HTTP server.

The load tool is the evidence for US-304 AC4/AC6 and US-305 AC4, so it is tested for the two
mistakes that would make that evidence wrong: passing while clients got errors, and declaring a
rollback finished while the canary still answers. Its latency report is the client-side evidence
for the v0.3 NFR (REST API P99 < 300 ms), so a slow backend must fail --max-p99.
"""

import os
import subprocess
import sys
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gateway_load.py")


class Backend(BaseHTTPRequestHandler):
    """Answers like a gateway pod; the test sets which track answers and whether it fails."""

    track = "stable"
    fail_every = 0
    delay = 0
    served = 0
    lock = threading.Lock()

    def do_GET(self):  # noqa: N802 (http.server naming)
        with Backend.lock:
            Backend.served += 1
            failing = Backend.fail_every and Backend.served % Backend.fail_every == 0
            track = Backend.track
        time.sleep(Backend.delay)
        status = 503 if failing else 200
        self.send_response(status)
        self.send_header("X-EOP-Version", "0.3.1" if track == "canary" else "0.3.0")
        self.send_header("X-EOP-Track", track)
        self.send_header("Content-Length", "2")
        self.end_headers()
        self.wfile.write(b"{}")

    def log_message(self, *_):
        pass


class GatewayLoadTest(unittest.TestCase):
    def setUp(self):
        Backend.track, Backend.fail_every, Backend.delay, Backend.served = "stable", 0, 0, 0
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Backend)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()

    def run_tool(self, *args, timeout=30):
        return subprocess.run([sys.executable, TOOL, "--url", self.url, *args],
                              capture_output=True, text=True, timeout=timeout, check=False)

    def test_clean_run_passes_and_reports_the_split(self):
        out = self.run_tool("--duration", "1", "--report-every", "0.5", "--concurrency", "2")
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn("0.3.0 (stable)", out.stdout)
        self.assertIn("OK: no 5xx and no connection errors", out.stdout)

    def test_latency_is_reported_per_path_and_meets_the_limit(self):
        out = self.run_tool("--duration", "1", "--concurrency", "2", "--paths", "/v1/health,/v1/nodes",
                            "--max-p99", "300")
        self.assertEqual(out.returncode, 0, out.stdout)
        self.assertIn("latency /v1/health: n=", out.stdout)
        self.assertIn("latency /v1/nodes: n=", out.stdout)
        self.assertIn("OK: p99 of every path under 300 ms", out.stdout)

    def test_a_p99_over_the_limit_fails_the_run(self):
        Backend.delay = 0.05
        out = self.run_tool("--duration", "1", "--concurrency", "2", "--max-p99", "20")
        self.assertEqual(out.returncode, 1, out.stdout)
        self.assertIn("p99", out.stdout)
        self.assertIn("> 20 ms", out.stdout)
        # Slow is not an error: the 5xx check still passes on its own.
        self.assertIn("OK: no 5xx and no connection errors", out.stdout)

    def test_any_5xx_fails_the_run(self):
        Backend.fail_every = 7
        out = self.run_tool("--duration", "1", "--concurrency", "2")
        self.assertEqual(out.returncode, 1, out.stdout)
        self.assertIn("HTTP 503 from 0.3.0 (stable)", out.stdout)

    def test_connection_errors_fail_the_run(self):
        self.server.shutdown()
        self.server.server_close()
        out = self.run_tool("--duration", "1", "--concurrency", "1")
        self.assertEqual(out.returncode, 1, out.stdout)
        self.assertIn("connection-error", out.stdout)

    def test_until_absent_waits_for_the_track_to_disappear(self):
        Backend.track = "canary"
        started = time.time()
        threading.Timer(1.0, lambda: setattr(Backend, "track", "stable")).start()
        out = self.run_tool("--until-absent", "canary", "--since", str(started), "--settle", "10", "--timeout", "10")
        self.assertEqual(out.returncode, 0, out.stdout)
        # Reported from --since, so it includes the second the canary kept answering.
        reported = float(out.stdout.split("responses ")[1].split(" s")[0])
        self.assertGreaterEqual(reported, 1.0)
        self.assertLess(reported, 5.0)

    def test_until_absent_fails_when_the_track_keeps_answering(self):
        Backend.track = "canary"
        out = self.run_tool("--until-absent", "canary", "--timeout", "1")
        self.assertEqual(out.returncode, 1, out.stdout)
        self.assertIn("FAIL", out.stdout)


if __name__ == "__main__":
    unittest.main()
