#!/usr/bin/env python3
"""
Contract test for the EOP HPC Performance Grafana dashboard.

This test validates that the dashboard required by US-206 is versioned in the
repository and contains the minimum HPC performance panels and query contract.
It does not require a running Grafana instance.
"""

import json
import sys
from pathlib import Path


DASHBOARD_PATH = Path("monitoring/grafana/dashboards/eop-hpc-performance-dashboard.json")

REQUIRED_TITLES = {
    "Graphs Processed per Minute",
    "HPC Processing Latency P50/P95/P99",
    "Average HPC Processing Time",
    "Per-Algorithm Timing P95",
    "OpenMP Thread Count",
    "Graph Size",
    "OpenMP Speedup Ratio",
    "HPC Error Rate",
    "OpenMP Thread Utilization",
}

REQUIRED_QUERY_TOKENS = {
    "eop_hpc_requests_total",
    "histogram_quantile",
    "eop_hpc_processing_duration_seconds_bucket",
    "eop_hpc_processing_duration_seconds_sum",
    "eop_hpc_processing_duration_seconds_count",
    "eop_hpc_algorithm_duration_seconds_bucket",
    "eop_hpc_thread_count",
    "eop_hpc_graph_nodes",
    "eop_hpc_graph_edges",
    "eop_hpc_speedup_ratio",
    "eop_hpc_thread_busy_seconds_total",
    "eop_hpc_active_threads",
}


def fail(message: str) -> None:
    print(f"ERROR: {message}")
    sys.exit(1)


def load_dashboard() -> dict:
    try:
        with DASHBOARD_PATH.open(encoding="utf-8") as dashboard_file:
            return json.load(dashboard_file)
    except FileNotFoundError:
        fail(f"Dashboard file not found: {DASHBOARD_PATH}")
    except json.JSONDecodeError as exc:
        fail(f"Dashboard JSON is invalid: {exc}")

    raise AssertionError("unreachable")


def collect_prometheus_exprs(dashboard: dict) -> list[str]:
    exprs = []

    for panel in dashboard.get("panels", []):
        for target in panel.get("targets", []):
            expr = target.get("expr")
            if expr:
                exprs.append(expr)

    return exprs


def main() -> None:
    dashboard = load_dashboard()

    if dashboard.get("uid") != "eop-hpc-performance-dashboard":
        fail("Dashboard uid must be 'eop-hpc-performance-dashboard'.")

    if dashboard.get("title") != "EOP HPC Performance Dashboard":
        fail("Dashboard title must be 'EOP HPC Performance Dashboard'.")

    tags = set(dashboard.get("tags", []))
    for expected_tag in {"eop", "hpc", "performance", "us-206"}:
        if expected_tag not in tags:
            fail(f"Missing dashboard tag: {expected_tag}")

    time_range = dashboard.get("time", {})
    if time_range.get("from") != "now-24h":
        fail("Dashboard default time range must start at 'now-24h'.")

    panels = dashboard.get("panels", [])
    if not panels:
        fail("Dashboard must define at least one panel.")

    panel_titles = {panel.get("title") for panel in panels}
    missing_titles = sorted(REQUIRED_TITLES - panel_titles)
    if missing_titles:
        fail(f"Missing required dashboard panels: {', '.join(missing_titles)}")

    exprs = collect_prometheus_exprs(dashboard)
    if not exprs:
        fail("Dashboard must define Prometheus query expressions.")

    joined_exprs = "\n".join(exprs)
    missing_tokens = sorted(token for token in REQUIRED_QUERY_TOKENS if token not in joined_exprs)
    if missing_tokens:
        fail(f"Dashboard queries are missing required metric/query tokens: {', '.join(missing_tokens)}")

    print(
        "[hpc-dashboard-test] contract passed: "
        f"dashboard={DASHBOARD_PATH} panels={len(panels)} queries={len(exprs)}"
    )


if __name__ == "__main__":
    main()
