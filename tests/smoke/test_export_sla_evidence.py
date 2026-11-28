#!/usr/bin/env python3
"""Unit tests of scripts/export_sla_evidence.py against a fake Grafana and Prometheus (`make sla-export-tool-test`)."""

import csv
import http.server
import json
import os
import re
import sys
import tempfile
import threading
import unittest
import urllib.parse

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import export_sla_evidence as tool  # noqa: E402

DASHBOARD = {
    "uid": tool.DASHBOARD_UID,
    "panels": [
        {"id": 1, "type": "row", "title": "SLO"},
        {"id": 2, "type": "stat", "title": "Error Budget Remaining (30 d)",
         "targets": [{"refId": "A", "expr": "min(last_over_time(eop:error_budget_remaining:ratio30d[2h])) * 100"}]},
        {"id": 3, "type": "stat", "title": "MTTR P99 (dashboard range)",
         "targets": [{"refId": "A", "expr": "histogram_quantile(0.99, sum by (le) (increase(x_bucket[$__range])))"}]},
        {"id": 4, "type": "text", "title": "Notes"},
    ],
}


class Fake(http.server.BaseHTTPRequestHandler):
    """Grafana and Prometheus on one port: enough of both APIs for the tool."""
    queries = []
    render = False
    budget = "0.42"

    def log_message(self, *args):
        pass

    def reply(self, code, body, content_type="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        params = dict(urllib.parse.parse_qsl(url.query))
        if url.path == f"/api/dashboards/uid/{tool.DASHBOARD_UID}":
            if self.headers.get("Authorization") is None:
                return self.reply(401, {"message": "unauthorized"})
            return self.reply(200, {"dashboard": DASHBOARD})
        if url.path == "/api/v1/query_range":
            Fake.queries.append(params)
            return self.reply(200, {"status": "success", "data": {"result": [
                {"metric": {"__name__": "m", "service": "eop-server"}, "values": [[1000, "1"], [4600, "2"]]}]}})
        if url.path == "/api/v1/query":
            value = Fake.budget if "error_budget" in params["query"] else "0.999"
            if "replica_replacement" in params["query"]:
                return self.reply(200, {"status": "success", "data": {"result": []}})
            return self.reply(200, {"status": "success", "data": {"result": [{"metric": {}, "value": [1, value]}]}})
        if url.path.startswith("/render/"):
            if Fake.render:
                return self.reply(200, b"\x89PNG fake", "image/png")
            return self.reply(500, {"message": "no renderer"})
        return self.reply(404, {})


class ExportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fake)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.url = f"http://127.0.0.1:{cls.server.server_address[1]}"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()

    def setUp(self):
        Fake.queries.clear()
        Fake.render = False
        Fake.budget = "0.42"
        self.out = tempfile.mkdtemp()

    def run_tool(self):
        return tool.main(["--grafana", self.url, "--prometheus", self.url, "--out", self.out,
                          "--days", "30", "--step", "1h", "--end", "2592000"])

    def test_exports_dashboard_panels_and_summary(self):
        self.assertEqual(self.run_tool(), 0)
        files = sorted(os.listdir(self.out))
        self.assertIn("dashboard.json", files)
        self.assertIn("02-error-budget-remaining-30-d.csv", files)
        self.assertIn("03-mttr-p99-dashboard-range.csv", files)
        self.assertNotIn("04-notes.csv", files, "panels without queries are skipped")

        with open(os.path.join(self.out, "02-error-budget-remaining-30-d.csv")) as handle:
            rows = list(csv.reader(handle))
        self.assertEqual(rows[0], ["query", "series", "timestamp", "value"])
        self.assertEqual(rows[1], ["A", "service=eop-server", "1000", "1"])

        # $__range resolves to the period; the step is one hour; the period ends at --end.
        self.assertIn("[30d]", Fake.queries[1]["query"])
        self.assertEqual(Fake.queries[0]["step"], "3600")
        self.assertEqual(float(Fake.queries[0]["end"]), 2592000)
        self.assertEqual(float(Fake.queries[0]["start"]), 0)

        with open(os.path.join(self.out, "summary.json")) as handle:
            summary = json.load(handle)
        self.assertEqual(summary["error_budget_remaining_30d"], 0.42)
        self.assertIsNone(summary["replica_replacement_last_seconds"], "no data is null, not 0")

    def test_screenshot_falls_back_to_instructions(self):
        self.run_tool()
        self.assertFalse(os.path.exists(os.path.join(self.out, "dashboard.png")))
        with open(os.path.join(self.out, "SCREENSHOT.txt")) as handle:
            text = handle.read()
        self.assertIn("grafana-image-renderer", text)
        self.assertIn(f"/d/{tool.DASHBOARD_UID}?from=0&to=2592000000", text)

    def test_screenshot_with_the_renderer(self):
        Fake.render = True
        self.run_tool()
        with open(os.path.join(self.out, "dashboard.png"), "rb") as handle:
            self.assertTrue(handle.read().startswith(b"\x89PNG"))

    def test_an_exhausted_budget_fails(self):
        Fake.budget = "0"
        self.assertEqual(self.run_tool(), 1)

    def test_summary_reads_recording_rules_through_a_range(self):
        # One sample an hour: an instant read of the rule (5-minute lookback) would come back empty.
        for key, query in tool.SUMMARY_QUERIES.items():
            for rule in re.findall(r"eop:[a-z0-9_:]+", query):
                self.assertIn(f"last_over_time({rule}", query, key)

    def test_helpers(self):
        self.assertEqual(tool.duration_seconds("1h"), 3600)
        self.assertEqual(tool.duration_seconds("30d"), 30 * 86400)
        with self.assertRaises(ValueError):
            tool.duration_seconds("1 hour")
        self.assertEqual(tool.substitute("rate(x[$__rate_interval]) [$__range] $__interval", 30, "1h"),
                         "rate(x[5m]) [30d] 1h")
        self.assertEqual(tool.slug("MTTR P99 (dashboard range)"), "mttr-p99-dashboard-range")
        self.assertEqual(tool.series_name({"__name__": "up"}), "up")


if __name__ == "__main__":
    unittest.main(verbosity=1)
