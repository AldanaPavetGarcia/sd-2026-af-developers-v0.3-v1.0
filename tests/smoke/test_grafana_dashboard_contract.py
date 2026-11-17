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

import yaml


DASHBOARD_PATH = Path("monitoring/grafana/dashboards/eop-slo-dashboard.json")
PROVIDER_PATH = Path("monitoring/grafana/provisioning/dashboards/eop-dashboard-provider.yaml")
DATASOURCE_PATH = Path("monitoring/grafana/provisioning/datasources/prometheus-datasource.yaml")
GATEWAY_ALERTS_PATH = Path("monitoring/grafana/provisioning/alerting/eop-gateway-alerts.yaml")
GRAFANA_CONFIGMAP_PATH = Path("k8s/grafana-provisioning-configmap.yaml")
GRAFANA_DEPLOYMENT_PATH = Path("k8s/grafana-deployment.yaml")
COMPOSE_PATH = Path("docker-compose.yml")

REQUIRED_TITLES = {
    "Service Availability",
    "Requests per Second",
    "P99 Request Latency",
    "Error Rate",
    "Error Budget Remaining",
    "Average Request Latency",
    "Failed Requests",
    # US-303 AC5: circuit breaker state of the API gateway's external services.
    "External Service Circuit Breakers",
    "Circuit Breaker Fallbacks and Transitions",
    # US-304 AC3: error rate, P99 and RPS of the API gateway per version during a rollout.
    "Gateway Error Rate by Version",
    "Gateway P99 Latency by Version",
    "Gateway Requests per Second by Version",
    # US-407 AC4 / ADR-027: MTTR on Pod failure, with the replica replacement time next to it.
    "MTTR on Pod Failure (service restoration)",
    "Replica Replacement Time (pod failure to pod Ready)",
    "MTTR P99 and Routable eop-server Replicas",
}

REQUIRED_QUERY_TOKENS = {
    "up",
    "eop_requests_total",
    "histogram_quantile",
    "eop_request_duration_seconds_bucket",
    "eop_request_duration_seconds_sum",
    "eop_request_duration_seconds_count",
    "eop_gateway_circuit_state",
    "eop_gateway_fallback_responses_total",
    "eop_circuit_breaker_state_transitions_total",
    "eop_gateway_http_requests_total",
    "eop_gateway_http_request_duration_seconds_bucket",
    "by (version, track)",
    "eop_mttr_last_seconds",
    "eop_mttr_seconds_bucket",
    "eop_server_routable_replicas",
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


def validate_canary_alert() -> None:
    """US-304 AC4: the canary rollback trigger is versioned, provisioned and means what it says."""
    alerts = yaml.safe_load(read_required_file(GATEWAY_ALERTS_PATH))
    rules = {rule["uid"]: rule for group in alerts.get("groups", []) for rule in group.get("rules", [])}
    rule = rules.get("eop-gateway-canary-error-rate")
    if rule is None:
        fail("Missing alert rule eop-gateway-canary-error-rate.")

    data = {item["refId"]: item["model"] for item in rule["data"]}
    expr = data.get("query", {}).get("expr", "")
    for token in ('track="canary"', 'status_class="5xx"', "[5m]", "eop_gateway_http_requests_total"):
        if token not in expr:
            fail(f"Canary alert query is missing {token!r}: {expr}")
    evaluator = data.get("threshold", {}).get("conditions", [{}])[0].get("evaluator", {})
    if evaluator != {"type": "gt", "params": [1]}:
        fail(f"Canary alert must fire above 1 %, got {evaluator}")
    if rule.get("noDataState") != "OK":
        fail("Canary alert must not fire when there is no canary traffic (noDataState: OK).")

    configmap = yaml.safe_load(read_required_file(GRAFANA_CONFIGMAP_PATH))
    copy = configmap.get("data", {}).get(GATEWAY_ALERTS_PATH.name)
    if copy is None or yaml.safe_load(copy) != alerts:
        fail(f"{GRAFANA_CONFIGMAP_PATH} must carry the same {GATEWAY_ALERTS_PATH.name} as {GATEWAY_ALERTS_PATH}.")
    mount = f"/etc/grafana/provisioning/alerting/{GATEWAY_ALERTS_PATH.name}"
    for path in (GRAFANA_DEPLOYMENT_PATH, COMPOSE_PATH):
        if mount not in read_required_file(path):
            fail(f"{path} does not mount {mount}.")


def main() -> None:
    dashboard = load_dashboard()

    validate_dashboard(dashboard)
    validate_dashboard_provider()
    validate_prometheus_datasource()
    validate_canary_alert()

    print(
        "[slo-dashboard-test] contract passed: "
        f"dashboard={DASHBOARD_PATH} "
        f"provider={PROVIDER_PATH} "
        f"datasource={DATASOURCE_PATH} "
        f"panels={len(dashboard.get('panels', []))}"
    )


if __name__ == "__main__":
    main()
