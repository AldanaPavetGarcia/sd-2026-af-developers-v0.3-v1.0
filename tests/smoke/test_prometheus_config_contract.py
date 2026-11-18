#!/usr/bin/env python3
"""
Contract test for the Prometheus scrape configuration (#43).

This test validates that both Prometheus configs in the repo scrape eop-server's /metrics
endpoint (#42) on the port/hostname the dashboards (#38, #39) and their Grafana provisioning
already expect. The Kubernetes one uses a single unified job (US-205 AC4: "Prometheus scrapes
metrics from all pods via K8s annotations") that discovers both eop-server and hpc-engine (#46)
via their prometheus.io/scrape|path|port pod annotations — docker-compose keeps eop-server on a
static target instead, since it's the only workload that runs there (hpc-engine is K8s-only,
see #16). It does not require Prometheus, Grafana, or eop-server to be running — same
static-contract approach as test_grafana_dashboard_contract.py / test_hpc_dashboard_contract.py,
deliberately checking for required tokens rather than parsing full YAML (no new dependency).
"""

import sys
from pathlib import Path


COMPOSE_CONFIG_PATH = Path("monitoring/prometheus/prometheus.yml")
K8S_CONFIGMAP_PATH = Path("k8s/prometheus-configmap.yaml")
# 30-day SLO run (#67): persistent TSDB, 35-day retention and the hourly recording rules.
K8S_DEPLOYMENT_PATH = Path("k8s/prometheus-deployment.yaml")
K8S_PVC_PATH = Path("k8s/prometheus-pvc.yaml")
K8S_RULES_PATH = Path("k8s/prometheus-rules-configmap.yaml")

COMPOSE_REQUIRED_TOKENS = [
    "scrape_configs:",
    "job_name: eop-server",
    "eop-server:9100",
]

K8S_REQUIRED_TOKENS = [
    "scrape_configs:",
    "kubernetes_sd_configs:",
    "role: pod",
    "prometheus_io_scrape",
    "prometheus_io_path",
    "prometheus_io_port",
    "rule_files:",
    "/etc/prometheus/rules/*.yml",
]

K8S_DEPLOYMENT_REQUIRED_TOKENS = [
    "--storage.tsdb.path=/prometheus",
    "--storage.tsdb.retention.time=35d",
    "--storage.tsdb.retention.size=",
    "claimName: prometheus-data",
    "name: eop-prometheus-rules",
    "mountPath: /etc/prometheus/rules",
    "type: Recreate",
]

K8S_PVC_REQUIRED_TOKENS = ["kind: PersistentVolumeClaim", "name: prometheus-data", "ReadWriteOnce"]

K8S_RULES_REQUIRED_TOKENS = [
    "name: eop-prometheus-rules",
    "interval: 1h",
    "record: eop:requests:increase1h",
    "record: eop:requests_ok:increase1h",
    "record: eop:error_budget_remaining:ratio30d",
    "record: eop:gateway_success:ratio30d",
    "record: eop:up:avg30d",
]


def fail(message: str) -> None:
    print(f"ERROR: {message}")
    sys.exit(1)


def read_required_file(path: Path) -> str:
    if not path.exists():
        fail(f"Required file not found: {path}")
    return path.read_text(encoding="utf-8")


def validate_tokens(config: str, tokens: list[str], label: str) -> None:
    for token in tokens:
        if token not in config:
            fail(f"{label} is missing required token: {token}")

    if "global:" not in config or "scrape_interval:" not in config:
        fail(f"{label} must define a global scrape_interval.")


def main() -> None:
    compose_config = read_required_file(COMPOSE_CONFIG_PATH)
    validate_tokens(compose_config, COMPOSE_REQUIRED_TOKENS, "docker-compose Prometheus config")

    k8s_config = read_required_file(K8S_CONFIGMAP_PATH)
    validate_tokens(k8s_config, K8S_REQUIRED_TOKENS, "Kubernetes Prometheus ConfigMap")

    for path, tokens in (
        (K8S_DEPLOYMENT_PATH, K8S_DEPLOYMENT_REQUIRED_TOKENS),
        (K8S_PVC_PATH, K8S_PVC_REQUIRED_TOKENS),
        (K8S_RULES_PATH, K8S_RULES_REQUIRED_TOKENS),
    ):
        text = read_required_file(path)
        for token in tokens:
            if token not in text:
                fail(f"{path} is missing required token: {token}")

    print(
        "[prometheus-test] contract passed: "
        f"compose={COMPOSE_CONFIG_PATH} k8s={K8S_CONFIGMAP_PATH}"
    )


if __name__ == "__main__":
    main()
