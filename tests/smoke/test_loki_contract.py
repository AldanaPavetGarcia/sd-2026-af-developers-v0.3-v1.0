#!/usr/bin/env python3
"""Static contract test for centralized logging (US-306, ADR-026).

Checks, without a running cluster, the parts of US-306 that live in versioned files:
retention as code (AC4, AC8), the log agent's pipeline (AC1, AC2, AC5), the Grafana Loki
datasource and its link to SigNoz (AC3), and the request-logs dashboard (AC1).
"""

import json
import sys
from pathlib import Path

import yaml

LOKI_CONFIG = Path("monitoring/loki/config.yaml")
AGENT_CONFIG = Path("monitoring/otel-log-agent/config.yaml")
DATASOURCE = Path("monitoring/grafana/provisioning/datasources/loki-datasource.yaml")
K8S_PROVISIONING = Path("k8s/grafana-provisioning-configmap.yaml")
DASHBOARD = Path("monitoring/grafana/dashboards/eop-request-logs.json")
SEVEN_DAYS_HOURS = 7 * 24


def fail(message):
    print(f"[loki-test] FAIL: {message}")
    sys.exit(1)


def load_yaml(path):
    if not path.is_file():
        fail(f"required file not found: {path}")
    return yaml.safe_load(path.read_text())


def hours(duration):
    units = {"h": 1, "d": 24, "w": 168}
    if not duration or duration[-1] not in units or not duration[:-1].isdigit():
        fail(f"unsupported duration {duration!r}")
    return int(duration[:-1]) * units[duration[-1]]


def validate_loki():
    config = load_yaml(LOKI_CONFIG)
    retention = config.get("limits_config", {}).get("retention_period", "")
    if hours(retention) < SEVEN_DAYS_HOURS:
        fail(f"retention_period {retention!r} is shorter than 7 days (US-306 AC4)")
    if not config.get("compactor", {}).get("retention_enabled"):
        fail("compactor.retention_enabled must be true, or retention is never enforced (AC8)")
    if not config.get("limits_config", {}).get("allow_structured_metadata"):
        fail("allow_structured_metadata must be true: trace_id and level are structured metadata")
    if config.get("common", {}).get("path_prefix") != "/loki":
        fail("Loki data must live under /loki, the persistent volume (AC4)")


def validate_agent():
    config = load_yaml(AGENT_CONFIG)
    filelog = config.get("receivers", {}).get("filelog", {})
    if "/var/log/pods/*/*/*.log" not in filelog.get("include", []):
        fail("the agent must read every pod's log files (AC1, AC5)")
    if filelog.get("storage") != "file_storage":
        fail("read offsets must be checkpointed in file_storage (AC4)")
    operator_types = [op.get("type") for op in filelog.get("operators", [])]
    for required in ("container", "json_parser", "trace_parser", "severity_parser"):
        if required not in operator_types:
            fail(f"agent pipeline is missing the {required} operator")
    exporter = config.get("exporters", {}).get("otlphttp/loki", {})
    if exporter.get("endpoint") != "http://loki:3100/otlp":
        fail("logs must be exported to Loki's OTLP endpoint")
    pipeline = config.get("service", {}).get("pipelines", {}).get("logs", {})
    if pipeline.get("exporters") != ["otlphttp/loki"] or "k8sattributes" not in pipeline.get("processors", []):
        fail("logs pipeline must enrich with k8sattributes and export to Loki")


def validate_datasource():
    datasource = load_yaml(DATASOURCE)
    loki = next((d for d in datasource.get("datasources", []) if d.get("type") == "loki"), None)
    if not loki or loki.get("uid") != "loki" or loki.get("url") != "http://loki:3100":
        fail("Loki datasource must have uid 'loki' and url http://loki:3100")
    fields = loki.get("jsonData", {}).get("derivedFields", [])
    if not any(f.get("matcherRegex") == "trace_id" and "/trace/" in f.get("url", "") for f in fields):
        fail("trace_id must link to the SigNoz trace view (AC3)")
    k8s_copy = load_yaml(K8S_PROVISIONING).get("data", {}).get("loki-datasource.yaml")
    if k8s_copy is None or yaml.safe_load(k8s_copy) != datasource:
        fail("k8s/grafana-provisioning-configmap.yaml must carry the same Loki datasource")


def validate_dashboard():
    if not DASHBOARD.is_file():
        fail(f"required file not found: {DASHBOARD}")
    dashboard = json.loads(DASHBOARD.read_text())
    variables = {v.get("name") for v in dashboard.get("templating", {}).get("list", [])}
    for required in ("service", "level", "trace_id"):
        if required not in variables:
            fail(f"dashboard must let the operator filter by {required} (AC1)")
    exprs = "\n".join(t.get("expr", "") for p in dashboard.get("panels", []) for t in p.get("targets", []))
    for token in ("service_name", "level", "trace_id"):
        if token not in exprs:
            fail(f"dashboard queries never filter on {token}")
    if any(p.get("datasource", {}).get("uid") != "loki" for p in dashboard.get("panels", [])):
        fail("every panel must use the Loki datasource")


def main():
    validate_loki()
    validate_agent()
    validate_datasource()
    validate_dashboard()
    print(f"[loki-test] contract passed: loki={LOKI_CONFIG} agent={AGENT_CONFIG} datasource={DATASOURCE} dashboard={DASHBOARD}")


if __name__ == "__main__":
    main()
