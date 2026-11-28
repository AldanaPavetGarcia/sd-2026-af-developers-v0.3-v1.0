#!/usr/bin/env python3
"""Unit tests of scripts/slo_partial_report.py on saved Prometheus answers (`make slo-partial-report-tool-test`)."""

import json
import os
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import slo_partial_report as tool  # noqa: E402

FIRST = 1795567620  # an evaluation at minute 47, as on the lab machine
HOURS = 6


def matrix(rows):
    return {"status": "success", "data": {"resultType": "matrix", "result": rows}}


def row(metric, values, skip=()):
    return {"metric": metric,
            "values": [[FIRST + i * 3600 + 0.5, str(v)] for i, v in enumerate(values) if i not in skip]}


class PartialReport(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.raw = os.path.join(self.tmp.name, "raw")
        self.out = os.path.join(self.tmp.name, "out")
        os.makedirs(self.raw)
        server = {"__name__": "eop:requests:increase1h", "service": "eop-server"}
        # Hour 3 has no sample (a gap); one failed command in hour 1.
        self.save("eop:requests:increase1h", [row(server, [1000] * HOURS, skip={3})])
        self.save("eop:requests_ok:increase1h", [row(server, [1000, 999, 1000, 1000, 1000, 1000], skip={3})])
        self.save("eop:gateway_requests:increase1h", [row({}, [3600] * HOURS, skip={3})])
        self.save("eop:gateway_requests_5xx:increase1h", [])
        self.save("eop:up:avg1h", [row({"job": "eop-server"}, [1, 1, 0.5, 1, 1, 1], skip={3}),
                                   row({"job": "slo-traffic"}, [1] * HOURS, skip={3})])
        self.save("eop:requests_success:ratio30d", [row({"service": "eop-server"}, [1, 0.9995, 0.9998, 0, 0.9998, 0.9998], skip={3})])
        self.save("eop:error_budget_remaining:ratio30d", [])
        self.save("eop:gateway_success:ratio30d", [])
        self.save("eop:up:avg30d", [row({"job": "eop-server"}, [1] * HOURS, skip={3})])

    def tearDown(self):
        self.tmp.cleanup()

    def save(self, rule, rows):
        with open(os.path.join(self.raw, tool.file_name(rule)), "w") as handle:
            json.dump(matrix(rows), handle)

    def run_tool(self, *extra):
        return tool.main(["--raw", self.raw, "--out", self.out, *extra])

    def report(self):
        with open(os.path.join(self.out, "slo-report.json")) as handle:
            return json.load(handle)

    def test_figures_of_the_period(self):
        self.assertEqual(self.run_tool("--start", "2026-11-19 00:47 UTC"), 0)
        report = self.report()
        self.assertEqual(report["start"], "2026-11-19 00:47 UTC")
        self.assertEqual((report["hours_recorded"], report["hours_expected"]), (5, 6))
        self.assertEqual(len(report["hours_missing"]), 1)
        server = report["eop_server"]["eop-server"]
        self.assertEqual((server["requests"], server["failed"], server["requests_per_hour"]), (5000, 1, 1000))
        self.assertAlmostEqual(server["success_ratio"], 0.9998)
        self.assertAlmostEqual(server["error_budget_remaining"], 0.8)
        self.assertEqual(report["availability"]["eop-server"], 0.9)
        self.assertEqual(report["availability"]["slo-traffic"], 1)
        self.assertEqual(report["recorded_30d"]["eop:requests_success:ratio30d"], {"eop-server": 0.9998})

    def test_no_5xx_series_counts_as_zero_and_says_so(self):
        self.run_tool()
        report = self.report()
        self.assertEqual(report["gateway"], {"requests": 18000, "5xx": 0, "success_ratio": 1.0})
        self.assertTrue(any("5xx" in note for note in report["notes"]))
        with open(os.path.join(self.out, "slo-report.md")) as handle:
            text = handle.read()
        self.assertIn("| API gateway success ratio (no 5xx) | 100.000 %", text)
        self.assertIn("5 of 6 hours recorded", text)

    def test_gateway_5xx_are_counted(self):
        self.save("eop:gateway_requests_5xx:increase1h", [row({}, [36] * HOURS, skip={3})])
        self.run_tool()
        self.assertAlmostEqual(self.report()["gateway"]["success_ratio"], 0.99)

    def test_series_file_has_the_export_shape(self):
        self.run_tool()
        with open(os.path.join(self.out, "slo-series.json")) as handle:
            series = json.load(handle)
        self.assertEqual(series["step_seconds"], 3600)
        self.assertEqual(set(series["series"]), set(tool.RULES))
        self.assertEqual(len(series["series"]["eop:requests:increase1h"][0]["values"]), 5)

    def test_missing_file_is_empty_and_no_requests_fails(self):
        os.remove(os.path.join(self.raw, tool.file_name("eop:up:avg30d")))
        self.assertEqual(self.run_tool(), 0)
        self.save("eop:requests:increase1h", [])
        self.assertEqual(self.run_tool(), 1)

    def test_rejects_an_instant_answer(self):
        with open(os.path.join(self.raw, tool.file_name("eop:up:avg1h")), "w") as handle:
            json.dump({"status": "success", "data": {"resultType": "vector", "result": []}}, handle)
        with self.assertRaises(ValueError):
            self.run_tool()


if __name__ == "__main__":
    unittest.main(verbosity=1)
