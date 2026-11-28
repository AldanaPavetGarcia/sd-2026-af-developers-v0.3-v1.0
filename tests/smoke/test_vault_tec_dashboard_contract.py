#!/usr/bin/env python3
"""
Contract test for the "Vault-Tec Delivery" Grafana dashboard (US-407 AC1-AC4, #75).

Static checks, no running Grafana or Prometheus:

* the dashboard covers the four parts of the SLA demo: 30-day SLO and error budget, MTTR, canary
  by version and the edge path;
* the 30-day row reads only recording rules that k8s/prometheus-rules-configmap.yaml defines, at
  one-hour granularity, over a 30-day default range;
* every raw metric it queries is exported by eop-server or the Go services;
* its thresholds are the SLO targets of config/production.env;
* it is provisioned in docker-compose, `make k8s-apply` and deploy/production.
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DASHBOARD_PATH = ROOT / "monitoring/grafana/dashboards/eop-vault-tec-delivery.json"
RULES_PATH = ROOT / "k8s/prometheus-rules-configmap.yaml"
ENV_PATH = ROOT / "config/production.env"
KUSTOMIZATION_PATH = ROOT / "monitoring/Kustomization"
COMPOSE_PATH = ROOT / "docker-compose.yml"
MAKEFILE_PATH = ROOT / "Makefile"
METRIC_SOURCES = [ROOT / "services/go/internal", ROOT / "src", ROOT / "hpc-engine/src"]

UID = "eop-vault-tec-delivery"
REQUIRED_TITLES = {
    # US-407 AC1/AC2: 30 days of SLO data, error budget above 0.
    "eop-server Success Ratio (30 d)",
    "Error Budget Remaining (30 d)",
    "API Gateway Success Ratio (30 d, no 5xx)",
    "eop-server Availability (30 d)",
    "Hourly Success Ratio (30 days)",
    "Error Budget Remaining over 30 days",
    # US-407 AC4: MTTR on a Pod deletion, next to the replacement time.
    "MTTR on Pod Failure (service restoration)",
    "MTTR P99 (dashboard range)",
    "Replica Replacement Time (pod failure to pod Ready)",
    "Routable eop-server Replicas",
    # US-407: live canary by version.
    "Gateway Requests per Second by Version",
    "Gateway Error Rate by Version",
    "Gateway P99 Latency by Version",
    # US-407: the edge path.
    "Edge E2E Latency P99",
    "Edge Devices Connected",
    "Edge Messages per Second",
}
# Panel title -> (production.env key, threshold value read from the panel).
THRESHOLDS = {
    "MTTR on Pod Failure (service restoration)": ("EOP_SLO_MTTR_SECONDS", 1.0),
    "MTTR P99 (dashboard range)": ("EOP_SLO_MTTR_SECONDS", 1.0),
    "Edge E2E Latency P99": ("EOP_SLO_EDGE_E2E_P99_SECONDS", 1.0),
    "eop-server Success Ratio (30 d)": ("EOP_SLO_AVAILABILITY_TARGET", 100.0),
    "API Gateway Success Ratio (30 d, no 5xx)": ("EOP_SLO_AVAILABILITY_TARGET", 100.0),
    "eop-server Availability (30 d)": ("EOP_SLO_AVAILABILITY_TARGET", 100.0),
}
RECORDING_RULE = re.compile(r"\beop:[a-z0-9_:]+")
RAW_METRIC = re.compile(r"\beop_[a-z0-9_]+")
# A recording rule followed by its optional labels and what comes next: it must be a range selector.
RULE_READ = re.compile(r"\b(eop:[a-z0-9_:]+)(\{[^}]*\})?\s*(.?)")
HISTOGRAM_SUFFIXES = ("_bucket", "_sum", "_count")
LIVE_RATE = re.compile(r"rate\(eop_(gateway|edge)_")


def fail(message: str) -> None:
    print(f"ERROR: {message}")
    sys.exit(1)


def exprs(panel):
    return [t.get("expr", "") for t in panel.get("targets", [])]


def defined_rules():
    return set(re.findall(r"record:\s*(\S+)", RULES_PATH.read_text()))


def env_values():
    values = {}
    for line in ENV_PATH.read_text().splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    return values


def exported(metric, sources):
    base = metric
    for suffix in HISTOGRAM_SUFFIXES:
        if base.endswith(suffix):
            base = base[: -len(suffix)]
            break
    return base in sources


def metric_sources():
    text = []
    for root in METRIC_SOURCES:
        for path in root.rglob("*"):
            if path.suffix in (".go", ".cpp", ".hpp") and not path.name.endswith("_test.go"):
                text.append(path.read_text(errors="replace"))
    return "\n".join(text)


def lowest_red(panel):
    steps = panel["fieldConfig"]["defaults"]["thresholds"]["steps"]
    for step in steps:
        if step["color"] == "red" and step["value"] is not None:
            return float(step["value"])
    # Success ratios: red below the first non-red step.
    for step in steps:
        if step["color"] != "red" and step["value"] is not None:
            return float(step["value"])
    return None


def main() -> None:
    if not DASHBOARD_PATH.exists():
        fail(f"missing {DASHBOARD_PATH.relative_to(ROOT)}")
    dashboard = json.loads(DASHBOARD_PATH.read_text())
    if dashboard.get("uid") != UID:
        fail(f"uid must be {UID}")
    if dashboard.get("time", {}).get("from") != "now-30d":
        fail("the default range must be the 30-day SLO window (now-30d)")

    panels = [p for p in dashboard["panels"] if p.get("type") != "row"]
    by_title = {p["title"]: p for p in panels}
    missing = REQUIRED_TITLES - set(by_title)
    if missing:
        fail(f"missing panels: {sorted(missing)}")
    ids = [p["id"] for p in dashboard["panels"]]
    if len(ids) != len(set(ids)):
        fail("panel ids must be unique")

    rules = defined_rules()
    sources = metric_sources()
    for panel in panels:
        for expr in exprs(panel):
            for rule in RECORDING_RULE.findall(expr):
                if rule not in rules:
                    fail(f"'{panel['title']}' reads {rule}, which no recording rule defines")
            # The rules record one sample an hour; an instant read (5-minute lookback) misses it.
            for rule, _, after in RULE_READ.findall(expr):
                if after != "[":
                    fail(f"'{panel['title']}' reads {rule} without a range selector "
                         f"(use last_over_time({rule}[2h]))")
            for metric in RAW_METRIC.findall(expr):
                if not exported(metric, sources):
                    fail(f"'{panel['title']}' queries {metric}, which nothing exports")
            if "$service" in expr:
                fail(f"'{panel['title']}' uses $service, but this dashboard has no variables")
        uses_rules = any(RECORDING_RULE.search(e) for e in exprs(panel))
        if panel["type"] == "timeseries" and uses_rules and panel.get("interval") != "1h":
            fail(f"'{panel['title']}' reads hourly recording rules: its interval must be 1h")
        # Canary and edge rates are live views: over the 30-day default range they would be flat.
        if LIVE_RATE.search(" ".join(exprs(panel))) and not panel.get("timeFrom"):
            fail(f"live panel '{panel['title']}' must override the 30-day range (timeFrom)")

    env = env_values()
    for title, (key, scale) in THRESHOLDS.items():
        if key not in env:
            fail(f"{key} missing from config/production.env")
        expected = float(env[key]) * scale
        got = lowest_red(by_title[title])
        if got is None or abs(got - expected) > 1e-9:
            fail(f"'{title}' threshold is {got}, config/production.env {key} gives {expected}")

    name = DASHBOARD_PATH.name
    if f"grafana/dashboards/{name}" not in KUSTOMIZATION_PATH.read_text():
        fail(f"monitoring/Kustomization does not generate {name} into eop-grafana-dashboards")
    if "./monitoring/grafana/dashboards/:/etc/grafana/provisioning/dashboards/eop/" not in COMPOSE_PATH.read_text():
        fail("docker-compose no longer mounts monitoring/grafana/dashboards/")
    if "--from-file=monitoring/grafana/dashboards/" not in MAKEFILE_PATH.read_text():
        fail("make k8s-apply no longer generates the dashboards ConfigMap from the directory")

    print(f"[vault-tec-dashboard-test] contract passed: dashboard={DASHBOARD_PATH.relative_to(ROOT)} "
          f"panels={len(panels)} rules={len(rules)}")


if __name__ == "__main__":
    main()
