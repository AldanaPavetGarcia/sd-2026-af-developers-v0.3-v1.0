#!/usr/bin/env python3
"""Unit tests of edge_load_test.py's comparison and dashboard reading (no stack needed)."""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import edge_load_test as e  # noqa: E402

ZERO = {"received": 0, "forwarded": 0, "invalid": 0, "duplicates": 0, "latency_count": 0}


def after(n, invalid=0):
    return {"received": n, "forwarded": n, "invalid": invalid, "duplicates": 0, "latency_count": n}


class SummarizeTest(unittest.TestCase):
    def test_a_clean_run_passes(self):
        summary, errors = e.summarize(ZERO, after(1500), 1500, 300.0, 5.1, 0.03, 0.09, 0.0)
        self.assertEqual(errors, [])
        self.assertEqual(summary["rate_real"], 5.0)

    def test_a_lost_message_fails(self):
        _, errors = e.summarize(ZERO, after(1499), 1500, 300.0, 5.0, 0.03, 0.09, 0.0)
        self.assertTrue(any("forwarded" in err for err in errors))

    def test_invalid_messages_fail(self):
        _, errors = e.summarize(ZERO, after(1500, invalid=2), 1500, 300.0, 5.0, 0.03, 0.09, 0.0)
        self.assertTrue(any("invalid" in err for err in errors))

    def test_a_panel_far_from_the_real_rate_fails(self):
        _, errors = e.summarize(ZERO, after(1500), 1500, 300.0, 6.0, 0.03, 0.09, 0.0)
        self.assertTrue(any("panel rate" in err for err in errors))

    def test_p99_at_the_limit_fails(self):
        _, errors = e.summarize(ZERO, after(1500), 1500, 300.0, 5.0, 0.03, 0.5, 0.0)
        self.assertTrue(any("P99" in err for err in errors))

    def test_loss_fails(self):
        _, errors = e.summarize(ZERO, after(1500), 1500, 300.0, 5.0, 0.03, 0.09, 0.2)
        self.assertTrue(any("loss" in err for err in errors))

    def test_counter_deltas_not_absolute_values(self):
        before = after(10_000)
        summary, errors = e.summarize(before, after(11_500), 1500, 300.0, 5.0, 0.03, 0.09, 0.0)
        self.assertEqual(errors, [])
        self.assertEqual(summary["forwarded"], 1500)


class DashboardTest(unittest.TestCase):
    def test_reads_the_panels_the_test_compares(self):
        exprs = e.panel_exprs()
        self.assertIn("eop_edge_messages_received_total", exprs[("Edge Messages per Second", "received")])
        self.assertIn("eop_edge_device_dropped", exprs[("Edge Message Loss Rate (buffer overflow)", "A")])


if __name__ == "__main__":
    unittest.main(verbosity=2)
