#!/usr/bin/env bash
set -euo pipefail

# End of the 30-day SLO run (#67): copies a consistent Prometheus TSDB snapshot out of the cluster
# and saves the hourly SLO series as JSON, into EOP_SLO_EXPORT_DIR (default slo-run-export/).
# The snapshot opens with: prometheus --storage.tsdb.path=<snapshot dir> --config.file=<any>.

NAMESPACE="${EOP_K8S_NAMESPACE:-default}"
LOCAL_PORT="${EOP_PROM_LOCAL_PORT:-19090}"
OUT="${EOP_SLO_EXPORT_DIR:-slo-run-export}"
k() { kubectl -n "${NAMESPACE}" "$@"; }
fail() { printf '[slo-export] FAIL: %s\n' "$*" >&2; exit 1; }

mkdir -p "${OUT}"
k port-forward deployment/prometheus "${LOCAL_PORT}:9090" >/dev/null 2>&1 &
pf=$!
trap 'kill ${pf} 2>/dev/null || true' EXIT
for _ in $(seq 1 30); do
    curl -sf "http://127.0.0.1:${LOCAL_PORT}/-/ready" >/dev/null 2>&1 && break
    sleep 0.5
done

snapshot="$(curl -sf -XPOST "http://127.0.0.1:${LOCAL_PORT}/api/v1/admin/tsdb/snapshot" \
    | python3 -c 'import json,sys; print(json.load(sys.stdin)["data"]["name"])')" \
    || fail "snapshot failed (is --web.enable-admin-api set?)"
pod="$(k get pods -l app=prometheus -o jsonpath='{.items[0].metadata.name}')"
printf '[slo-export] copying snapshot %s from %s...\n' "${snapshot}" "${pod}"
k cp "${pod}:/prometheus/snapshots/${snapshot}" "${OUT}/tsdb-${snapshot}" >/dev/null
tar -czf "${OUT}/tsdb-${snapshot}.tar.gz" -C "${OUT}" "tsdb-${snapshot}"
rm -rf "${OUT:?}/tsdb-${snapshot}"

PROM="http://127.0.0.1:${LOCAL_PORT}" OUT="${OUT}" python3 - <<'PY'
import json, os, time, urllib.parse, urllib.request

prom, out = os.environ["PROM"], os.environ["OUT"]
end = time.time()
start = end - 35 * 86400
series = {}
for name in ("eop:requests:increase1h", "eop:requests_ok:increase1h", "eop:gateway_requests:increase1h",
             "eop:gateway_requests_5xx:increase1h", "eop:up:avg1h", "eop:requests_success:ratio30d",
             "eop:error_budget_remaining:ratio30d", "eop:gateway_success:ratio30d", "eop:up:avg30d"):
    url = prom + "/api/v1/query_range?" + urllib.parse.urlencode(
        {"query": name, "start": start, "end": end, "step": "3600"})
    with urllib.request.urlopen(url, timeout=60) as resp:
        series[name] = json.load(resp)["data"]["result"]
path = os.path.join(out, "slo-series.json")
with open(path, "w", encoding="utf-8") as f:
    json.dump({"exported_at": int(end), "step_seconds": 3600, "series": series}, f)
print(f"[slo-export] hourly SLO series: {path}")
PY
ls -lh "${OUT}"
