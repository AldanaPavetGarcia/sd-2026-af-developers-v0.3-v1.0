#!/usr/bin/env bash
set -euo pipefail

# Is the 30-day SLO run (#67) collecting? Prints how many days of data Prometheus holds, whether
# the synthetic traffic is flowing and succeeding, and the 30-day SLO figures once they exist.
# Exits 1 if the TSDB is not on its volume or no traffic arrived in the last 10 minutes.

NAMESPACE="${EOP_K8S_NAMESPACE:-default}"
LOCAL_PORT="${EOP_PROM_LOCAL_PORT:-19090}"
k() { kubectl -n "${NAMESPACE}" "$@"; }
fail() { printf '[slo-run] FAIL: %s\n' "$*" >&2; exit 1; }

phase="$(k get pvc prometheus-data -o jsonpath='{.status.phase}' 2>/dev/null || true)"
[[ "${phase}" == "Bound" ]] || fail "PVC prometheus-data is '${phase:-missing}', not Bound"

k port-forward deployment/prometheus "${LOCAL_PORT}:9090" >/dev/null 2>&1 &
pf=$!
trap 'kill ${pf} 2>/dev/null || true' EXIT
for _ in $(seq 1 30); do
    curl -sf "http://127.0.0.1:${LOCAL_PORT}/-/ready" >/dev/null 2>&1 && break
    sleep 0.5
done

PROM="http://127.0.0.1:${LOCAL_PORT}" python3 - <<'PY'
import json, os, sys, time, urllib.parse, urllib.request

prom = os.environ["PROM"]

def query(expr):
    url = prom + "/api/v1/query?" + urllib.parse.urlencode({"query": expr})
    with urllib.request.urlopen(url, timeout=10) as resp:
        return json.load(resp)["data"]["result"]

def scalar(expr):
    result = query(expr)
    return float(result[0]["value"][1]) if result else None

lowest_ms = scalar("prometheus_tsdb_lowest_timestamp")
days = (time.time() * 1000 - lowest_ms) / 86_400_000 if lowest_ms else 0
print(f"[slo-run] data since {time.strftime('%Y-%m-%d %H:%M UTC', time.gmtime((lowest_ms or 0) / 1000))}"
      f" ({days:.1f} days of the 30)")

size = scalar("prometheus_tsdb_storage_blocks_bytes")
print(f"[slo-run] TSDB blocks on the volume: {(size or 0) / 2**20:.0f} MiB")

for row in query('up{job=~"eop-server|hpc-engine|api-gateway.*|slo-traffic"}'):
    print(f"[slo-run] up {row['metric'].get('job')} {row['metric'].get('pod', '')}: {row['value'][1]}")

sent = scalar("sum(increase(eop_traffic_requests_total[10m]))") or 0
failed = scalar('sum(increase(eop_traffic_requests_total{result="error"}[10m]))') or 0
nodes = scalar("eop_traffic_nodes_registered")
print(f"[slo-run] synthetic traffic, last 10 min: {sent:.0f} requests, {failed:.0f} failed; "
      f"bunker nodes registered: {nodes if nodes is not None else 'n/a'}")

for name in ("eop:requests_success:ratio30d", "eop:error_budget_remaining:ratio30d",
             "eop:gateway_success:ratio30d"):
    rows = query(name)
    shown = ", ".join(f"{r['metric'].get('service', 'all')}={float(r['value'][1]):.5f}" for r in rows)
    print(f"[slo-run] {name}: {shown or 'not recorded yet (first value after 1 h)'}")

if sent == 0:
    sys.exit("[slo-run] FAIL: no synthetic traffic in the last 10 minutes")
print("[slo-run] collecting")
PY
