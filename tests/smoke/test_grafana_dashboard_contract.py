#!/usr/bin/env python3
"""
Contract test for the EOP SLO Grafana dashboard.

This test validates that the dashboard required by ADR-009 / US-205 is
versioned in the repository and has provisioning files for Grafana.
It does not require a running Grafana instance.
"""

import json
import sys
from pathlib import Path


DASHBOARD_PATH = Path("monitoring/grafana/dashboards/eop-slo-dashboard.json")
PROVIDER_PATH = Path("monitoring/grafana/provisioning/dashboards/eop-dashboard-provider.yaml")
DATASOURCE_PATH = Path("monitoring/grafana/provisioning/datasources/prometheus-datasource.yaml")

REQUIRED_TITLES = {
    "Service Availability",
    "Requests per Second",
    "P99 Request Latency",
    "Error Rate",
    "Error Budget Remaining",
    "Average Request Latency",
    "Failed Requests",
}

REQUIRED_QUERY_TOKENS = {
    "up",
    "eop_requests_total",
    "histogram_quantile",
    "eop_request_duration_seconds_bucket",
    "eop_request_duration_seconds_sum",
    "eop_request_duration_seconds_count",
}


def fail(message: str) -> None:
    print(f"ERROR: {message}")
    sys.exit(1)


def read_required_file(path: Path) -> str:
    if not path.exists():
        fail(f"Required file not found: {path}")
    return path.read_text(encoding="utf-8")


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


def validate_dashboard(dashboard: dict) -> None:
    if dashboard.get("uid") != "eop-slo-dashboard":
        fail("Dashboard uid must be 'eop-slo-dashboard'.")

    if dashboard.get("title") != "EOP SLO Dashboard":
        fail("Dashboard title must be 'EOP SLO Dashboard'.")

    tags = set(dashboard.get("tags", []))
    for expected_tag in {"eop", "slo", "system-health"}:
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


def validate_dashboard_provider() -> None:
    provider = read_required_file(PROVIDER_PATH)

    required_tokens = [
        "apiVersion: 1",
        "providers:",
        "name: eop-dashboards",
        "type: file",
        "allowUiUpdates: false",
        "path: /etc/grafana/provisioning/dashboards/eop",
    ]

    for token in required_tokens:
        if token not in provider:
            fail(f"Dashboard provider is missing token: {token}")


def validate_prometheus_datasource() -> None:
    datasource = read_required_file(DATASOURCE_PATH)

    required_tokens = [
        "apiVersion: 1",
        "datasources:",
        "name: Prometheus",
        "uid: prometheus",
        "type: prometheus",
        "url: http://prometheus:9090",
    ]

    for token in required_tokens:
        if token not in datasource:
            fail(f"Prometheus datasource provisioning is missing token: {token}")


def main() -> None:
    dashboard = load_dashboard()

    validate_dashboard(dashboard)
    validate_dashboard_provider()
    validate_prometheus_datasource()

    print(
        "[slo-dashboard-test] contract passed: "
        f"dashboard={DASHBOARD_PATH} "
        f"provider={PROVIDER_PATH} "
        f"datasource={DATASOURCE_PATH} "
        f"panels={len(dashboard.get('panels', []))}"
    )


if __name__ == "__main__":
    main()
