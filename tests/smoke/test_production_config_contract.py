#!/usr/bin/env python3
"""Contract test of the production deployment (US-406 AC1, AC3; ADR-029). No cluster needed.

  - config/production.env has every key of the development ConfigMap (k8s/configmap.yaml) plus the
    production values, and the Grafana alert thresholds and base retention match it;
  - the Kustomization files list every manifest and every generated file, so nothing silently
    drops out of the product when a file is added;
  - deploy/production renders (with kubectl, when present) into the complete stack, with the
    retention arguments and the broker's TLS init container taken from the configuration.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
ENV_FILE = ROOT / "config/production.env"
DEV_CONFIGMAP = ROOT / "k8s/configmap.yaml"
ALERTS = ROOT / "monitoring/grafana/provisioning/alerting"
OVERLAY = ROOT / "deploy/production"

# Alert rule -> production.env key of its threshold.
ALERT_THRESHOLDS = {
    "eop-edge-alerts.yaml": "EOP_ALERT_EDGE_E2E_P99_SECONDS",
    "eop-gateway-alerts.yaml": "EOP_ALERT_CANARY_5XX_PERCENT",
    "eop-slo-alerts.yaml": "EOP_ALERT_ERROR_BUDGET_REMAINING_PERCENT",
}
REQUIRED_KEYS = {
    "EOP_PROMETHEUS_RETENTION", "EOP_PROMETHEUS_RETENTION_SIZE", "EOP_LOKI_RETENTION",
    "EOP_SLO_AVAILABILITY_TARGET", "EOP_SLO_SERVER_P99_SECONDS", "EOP_SLO_GATEWAY_P99_SECONDS",
    "EOP_SLO_EDGE_E2E_P99_SECONDS", "EOP_SLO_MTTR_SECONDS", "EOP_EDGE_LIVENESS_TIMEOUT",
    "EOP_EDGE_BROKER_PUBLIC_HOSTS", "EOP_EDGE_CERT_DAYS",
}
# Workloads the product must contain (kind, name).
REQUIRED_WORKLOADS = {
    ("Deployment", "eop-server"), ("Deployment", "redis"), ("Deployment", "api-gateway-stable"),
    ("Deployment", "ai-service"), ("Deployment", "corporate-service"), ("StatefulSet", "mosquitto"),
    ("Deployment", "edge-ingestor"), ("Deployment", "mttr-watcher"), ("Deployment", "prometheus"),
    ("Deployment", "grafana"), ("Deployment", "loki"), ("DaemonSet", "otel-log-agent"),
    ("Deployment", "otel-collector"), ("Deployment", "zookeeper-1"), ("Deployment", "clickhouse"),
    ("Deployment", "signoz"), ("Deployment", "signoz-otel-collector"),
    ("Job", "signoz-telemetrystore-migrator"),
}


def fail(message):
    print(f"[production-config-test] FAIL: {message}")
    sys.exit(1)


def read_env(path):
    values = {}
    for number, line in enumerate(path.read_text().splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            fail(f"{path.name}:{number}: not KEY=value")
        key, value = line.split("=", 1)
        if key in values:
            fail(f"{path.name}:{number}: {key} defined twice")
        values[key] = value
    return values


def threshold(alert_file):
    rules = yaml.safe_load(alert_file.read_text())
    for group in rules.get("groups", []):
        for rule in group.get("rules", []):
            for data in rule.get("data", []):
                model = data.get("model", {})
                if model.get("type") == "threshold":
                    return model["conditions"][0]["evaluator"]["params"][0]
    fail(f"{alert_file.name}: no threshold expression")
    return None


def listed(kustomization, key):
    data = yaml.safe_load(kustomization.read_text())
    if key == "resources":
        return set(data.get("resources", []))
    return {f for gen in data.get("configMapGenerator", []) for f in gen.get("files", [])}


def check_config(env):
    dev = yaml.safe_load(DEV_CONFIGMAP.read_text())["data"]
    missing = sorted(set(dev) - set(env))
    if missing:
        fail(f"production.env lacks keys of k8s/configmap.yaml: {missing}")
    missing = sorted(REQUIRED_KEYS - set(env))
    if missing:
        fail(f"production.env lacks production keys: {missing}")
    for name, key in ALERT_THRESHOLDS.items():
        value = threshold(ALERTS / name)
        if float(env[key]) != float(value):
            fail(f"{name} fires at {value}, production.env {key}={env[key]}")
    prometheus = (ROOT / "k8s/prometheus-deployment.yaml").read_text()
    if f"--storage.tsdb.retention.time={env['EOP_PROMETHEUS_RETENTION']}" not in prometheus:
        fail("the development Prometheus retention differs from production.env")


def check_listings():
    top = {p.name for p in (ROOT / "k8s").glob("*.yaml")} - {"configmap.yaml"}
    if listed(ROOT / "k8s/Kustomization", "resources") != top:
        fail("k8s/Kustomization must list every k8s/*.yaml except configmap.yaml")
    for directory in ("redis", "mosquitto", "external-stubs", "loki", "otel-log-agent"):
        files = {p.name for p in (ROOT / "k8s" / directory).glob("*.yaml")}
        if listed(ROOT / "k8s" / directory / "Kustomization", "resources") != files:
            fail(f"k8s/{directory}/Kustomization must list every manifest of its directory")
    generated = listed(ROOT / "monitoring/Kustomization", "files")
    for directory in ("grafana/dashboards", "signoz-common/clickhouse"):
        files = {f"{directory}/{p.name}" for p in (ROOT / "monitoring" / directory).iterdir() if p.is_file()}
        if not files <= generated:
            fail(f"monitoring/Kustomization does not generate {sorted(files - generated)}")


def check_render():
    if shutil.which("kubectl") is None:
        print("[production-config-test] kubectl not found: render check skipped")
        return
    out = subprocess.run(["kubectl", "kustomize", str(OVERLAY)], capture_output=True, text=True)
    if out.returncode != 0:
        fail(f"deploy/production does not render: {out.stderr.strip()}")
    docs = [d for d in yaml.safe_load_all(out.stdout) if d]
    present = {(d["kind"], d["metadata"]["name"]) for d in docs}
    missing = sorted(REQUIRED_WORKLOADS - present)
    if missing:
        fail(f"the product lacks {missing}")
    names = [(d["kind"], d["metadata"]["name"]) for d in docs]
    duplicates = sorted({n for n in names if names.count(n) > 1})
    if duplicates:
        fail(f"rendered twice: {duplicates}")
    by_name = {(d["kind"], d["metadata"]["name"]): d for d in docs}
    args = by_name[("Deployment", "prometheus")]["spec"]["template"]["spec"]["containers"][0]["args"]
    if "--storage.tsdb.retention.time=$(EOP_PROMETHEUS_RETENTION)" not in args:
        fail("Prometheus retention is not taken from production.env")
    args = by_name[("Deployment", "loki")]["spec"]["template"]["spec"]["containers"][0]["args"]
    if "-store.retention=$(EOP_LOKI_RETENTION)" not in args:
        fail("Loki retention is not taken from production.env")
    broker = by_name[("StatefulSet", "mosquitto")]["spec"]["template"]["spec"]
    if [c["command"] for c in broker.get("initContainers", [])] != [["/eop-certgen"]]:
        fail("the broker does not prepare its TLS material in the cluster")
    if not any(v.get("secret", {}).get("optional") for v in broker["volumes"]):
        fail("the operator's mosquitto-tls Secret must be optional")
    print(f"[production-config-test] deploy/production renders {len(docs)} resources")


def main():
    env = read_env(ENV_FILE)
    check_config(env)
    check_listings()
    check_render()
    print("[production-config-test] contract passed")


if __name__ == "__main__":
    os.chdir(ROOT)
    main()
