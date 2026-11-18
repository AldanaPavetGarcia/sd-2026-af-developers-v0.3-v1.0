#!/usr/bin/env bash
# Exports the SLA evidence of the deployed stack (US-407, #75): port-forwards Grafana and
# Prometheus from the current kubectl context and runs scripts/export_sla_evidence.py.
#
#   sla-export.sh [OUT_DIR]     default: evidence/sla-<UTC date>
#   SLA_DAYS=30 SLA_STEP=1h     period and granularity
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NAMESPACE="${EOP_K8S_NAMESPACE:-default}"
OUT="${1:-$ROOT/evidence/sla-$(date -u +%Y%m%d)}"
GRAFANA_PORT="${SLA_GRAFANA_PORT:-13000}"
PROMETHEUS_PORT="${SLA_PROMETHEUS_PORT:-19090}"
PIDS=()

cleanup() { for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done; }
trap cleanup EXIT

forward() {
    kubectl -n "$NAMESPACE" port-forward "svc/$1" "$2:$3" > /dev/null 2>&1 &
    PIDS+=("$!")
    for _ in $(seq 1 50); do
        if (exec 3<>"/dev/tcp/127.0.0.1/$2") 2> /dev/null; then return 0; fi
        sleep 0.2
    done
    echo "sla-export: port-forward to svc/$1 did not open" >&2
    exit 1
}

forward grafana "$GRAFANA_PORT" 3000
forward prometheus "$PROMETHEUS_PORT" 9090
python3 "$ROOT/scripts/export_sla_evidence.py" \
    --grafana "http://127.0.0.1:$GRAFANA_PORT" --prometheus "http://127.0.0.1:$PROMETHEUS_PORT" \
    --out "$OUT" --days "${SLA_DAYS:-30}" --step "${SLA_STEP:-1h}"
