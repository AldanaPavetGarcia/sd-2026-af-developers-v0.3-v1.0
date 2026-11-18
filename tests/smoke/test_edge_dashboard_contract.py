#!/usr/bin/env python3
"""
Contract test for the EOP Edge Telemetry dashboard and its alert (US-405 AC3, AC4, AC7).

Static checks, no running Grafana or Prometheus: the dashboard and the alert are versioned and
provisioned in docker-compose and Kubernetes, the alert and the P99 panel use the same
expression, and every metric the dashboard queries is one the edge ingestor exports.
"""

import json
import re
import sys
from pathlib import Path

import yaml

DASHBOARD_PATH = Path("monitoring/grafana/dashboards/eop-edge-dashboard.json")
ALERTS_PATH = Path("monitoring/grafana/provisioning/alerting/eop-edge-alerts.yaml")
GRAFANA_CONFIGMAP_PATH = Path("k8s/grafana-provisioning-configmap.yaml")
GRAFANA_DEPLOYMENT_PATH = Path("k8s/grafana-deployment.yaml")
COMPOSE_PATH = Path("docker-compose.yml")
INGESTOR_METRICS_PATH = Path("services/go/internal/ingest/metrics.go")

REQUIRED_TITLES = {
    "Edge Ingestor Broker Connection",
    "E2E Latency P99 (ESP32 to eop-server)",
    "Edge Message Loss Rate (buffer overflow)",
    "Edge Devices Connected",
    "Edge Messages per Second",
    "Edge E2E Latency (P50, P99)",
    "Edge Devices (broker connectivity)",
    "Device Reconnections, Buffer Deliveries and Retries",
    "Per-message traces in SigNoz",
}
P99_EXPR = "histogram_quantile(0.99, sum by (le) (rate(eop_edge_e2e_latency_seconds_bucket[1m])))"
P50_EXPR = "histogram_quantile(0.50, sum by (le) (rate(eop_edge_e2e_latency_seconds_bucket[1m])))"
LATENCY_LIMIT_S = 0.5


def fail(message: str) -> None:
    print(f"ERROR: {message}")
    sys.exit(1)


def read(path: Path) -> str:
    if not path.is_file():
        fail(f"Missing {path}.")
    return path.read_text(encoding="utf-8")


def exprs(dashboard: dict) -> list:
    return [t["expr"] for p in dashboard["panels"] for t in p.get("targets", []) if "expr" in t]


def check_dashboard(dashboard: dict) -> None:
    if dashboard.get("uid") != "eop-edge":
        fail("The edge dashboard uid must be eop-edge (linked from the delivery dashboard, US-405 AC5).")
    titles = {p.get("title") for p in dashboard.get("panels", [])}
    missing = REQUIRED_TITLES - titles
    if missing:
        fail(f"Missing panels: {sorted(missing)}")
    ids = [p.get("id") for p in dashboard["panels"]]
    if len(ids) != len(set(ids)):
        fail("Panel ids must be unique.")
    for panel in dashboard["panels"]:
        if panel.get("targets") and panel.get("datasource", {}).get("uid") != "prometheus":
            fail(f"Panel {panel['title']!r} must query the provisioned Prometheus datasource.")

    queries = exprs(dashboard)
    for expr in (P50_EXPR, P99_EXPR):
        if expr not in queries:
            fail(f"The dashboard must query {expr} (US-405 AC3).")
    if not any("eop_edge_messages_received_total" in q for q in queries):
        fail("Messages per second must come from eop_edge_messages_received_total.")
    if not any("eop_edge_device_dropped" in q for q in queries):
        fail("The loss rate must come from eop_edge_device_dropped (US-402 AC8).")

    table = next(p for p in dashboard["panels"] if p["title"] == "Edge Devices (broker connectivity)")
    table_queries = " ".join(t["expr"] for t in table["targets"])
    if "eop_edge_device_last_seen_timestamp_seconds" not in table_queries:
        fail("The device table must read eop_edge_device_last_seen_timestamp_seconds (US-405 AC7).")
    renamed = table["transformations"][1]["options"]["renameByName"].values()
    for column in ("Device", "Status", "Last seen"):
        if column not in renamed:
            fail(f"The device table needs a {column!r} column (connected / disconnected / last seen).")


def check_metrics_exist(dashboard: dict) -> None:
    exported = set(re.findall(r'Name:\s*"(eop_edge_[a-z0-9_]+)"', read(INGESTOR_METRICS_PATH)))
    exported |= set(re.findall(r'counter\("(eop_edge_[a-z0-9_]+)"', read(INGESTOR_METRICS_PATH)))
    for expr in exprs(dashboard):
        for name in re.findall(r"eop_edge_[a-z0-9_]+", expr):
            base = re.sub(r"_(bucket|sum|count)$", "", name)
            if base not in exported:
                fail(f"{name} is queried but the edge ingestor does not export {base}.")


def check_alert(alerts: dict) -> None:
    rules = {r["uid"]: r for g in alerts.get("groups", []) for r in g.get("rules", [])}
    rule = rules.get("eop-edge-e2e-p99")
    if rule is None:
        fail("Missing alert rule eop-edge-e2e-p99 (US-405 AC4).")
    data = {item["refId"]: item["model"] for item in rule["data"]}
    if data.get("query", {}).get("expr") != P99_EXPR:
        fail("The alert must use the same P99 expression as the dashboard.")
    params = data.get("threshold", {}).get("conditions", [{}])[0].get("evaluator", {})
    if params.get("type") != "gt" or params.get("params") != [LATENCY_LIMIT_S]:
        fail("The alert must fire above 0.5 s.")
    if rule.get("for") != "1m":
        fail("The alert must wait for 1m (more than 1 consecutive minute).")


def check_provisioning(alerts: dict) -> None:
    configmap = yaml.safe_load(read(GRAFANA_CONFIGMAP_PATH))
    copy = configmap.get("data", {}).get(ALERTS_PATH.name)
    if copy is None or yaml.safe_load(copy) != alerts:
        fail(f"{GRAFANA_CONFIGMAP_PATH} must carry the same {ALERTS_PATH.name} as {ALERTS_PATH}.")
    mount = f"/etc/grafana/provisioning/alerting/{ALERTS_PATH.name}"
    for path in (GRAFANA_DEPLOYMENT_PATH, COMPOSE_PATH):
        if mount not in read(path):
            fail(f"{path} does not mount {mount}.")
    # Dashboards are provisioned from the whole directory (compose bind mount, k8s-apply ConfigMap).
    if "./monitoring/grafana/dashboards/:/etc/grafana/provisioning/dashboards/eop/" not in read(COMPOSE_PATH):
        fail("docker-compose must mount monitoring/grafana/dashboards/.")


def main() -> None:
    dashboard = json.loads(read(DASHBOARD_PATH))
    alerts = yaml.safe_load(read(ALERTS_PATH))
    check_dashboard(dashboard)
    check_metrics_exist(dashboard)
    check_alert(alerts)
    check_provisioning(alerts)
    print(f"[edge-dashboard-test] contract passed: dashboard={DASHBOARD_PATH} panels={len(dashboard['panels'])} "
          f"alert={ALERTS_PATH}")


if __name__ == "__main__":
    main()
