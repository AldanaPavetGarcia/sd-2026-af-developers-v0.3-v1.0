#!/usr/bin/env bash
set -euo pipefail

# US-407 AC4 / ADR-027 §4: MTTR on an eop-server Pod failure must be below 200 ms.
#
# A probe Job (mttr-watcher probe) sends LIST_NODES through the eop-server Service every 20 ms
# from inside the cluster, so it goes through the Service's routing as any client does. Midway
# through, one eop-server Pod is deleted. The test fails if the probe saw an outage of 200 ms or
# more, or if mttr-watcher's service-restoration time for that Pod is 200 ms or more. The replica
# replacement time (pod failure -> new Pod Ready) is printed next to them.

NAMESPACE="${EOP_K8S_NAMESPACE:-default}"
LIMIT_MS="${EOP_MTTR_LIMIT_MS:-200}"
PROBE_SECONDS="${EOP_MTTR_PROBE_SECONDS:-40}"
DELETE_AFTER_SECONDS="${EOP_MTTR_DELETE_AFTER_SECONDS:-10}"
PROBE_IMAGE="${EOP_MTTR_IMAGE:-mttr-watcher:local}"
JOB="mttr-probe"

log() { printf '[mttr] %s\n' "$*" >&2; }
fail() { printf '[mttr] FAIL: %s\n' "$*" >&2; exit 1; }
k() { kubectl -n "${NAMESPACE}" "$@"; }

command -v kubectl >/dev/null || fail "kubectl is required"
command -v python3 >/dev/null || fail "python3 is required"

log "Waiting for eop-server (2 replicas) and mttr-watcher..."
k rollout status deployment/eop-server --timeout=180s >/dev/null
k rollout status deployment/mttr-watcher --timeout=120s >/dev/null
ready="$(k get deployment eop-server -o jsonpath='{.status.readyReplicas}')"
[[ "${ready}" -ge 2 ]] || fail "eop-server has ${ready} ready replicas; failover needs 2"

k delete job "${JOB}" --ignore-not-found --wait=true >/dev/null
log "Starting the probe (${PROBE_SECONDS} s, every 20 ms)..."
k apply -f - >/dev/null <<YAML
apiVersion: batch/v1
kind: Job
metadata:
  name: ${JOB}
  labels:
    app: mttr-probe
spec:
  backoffLimit: 0
  ttlSecondsAfterFinished: 600
  template:
    metadata:
      labels:
        app: mttr-probe
    spec:
      restartPolicy: Never
      securityContext:
        runAsNonRoot: true
        runAsUser: 65532
      containers:
        - name: probe
          image: ${PROBE_IMAGE}
          imagePullPolicy: IfNotPresent
          args: ["probe"]
          env:
            - name: EOP_MTTR_PROBE_ADDR
              value: "eop-server:9026"
            - name: EOP_MTTR_PROBE_DURATION
              value: "${PROBE_SECONDS}s"
YAML
k wait --for=condition=Ready pod -l job-name="${JOB}" --timeout=60s >/dev/null

sleep "${DELETE_AFTER_SECONDS}"
victim="$(k get pods -l app=eop-server -o jsonpath='{.items[0].metadata.name}')"
since="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
log "Deleting Pod ${victim}..."
k delete pod "${victim}" --wait=false >/dev/null

log "Waiting for the probe to finish..."
k wait --for=condition=Complete "job/${JOB}" --timeout="$((PROBE_SECONDS + 60))s" >/dev/null \
    || fail "the probe Job did not complete: $(k logs "job/${JOB}" 2>&1 | tail -5)"
k rollout status deployment/eop-server --timeout=180s >/dev/null

probe_json="$(k logs "job/${JOB}" | tail -n 1)"
watcher_logs="$(k logs deployment/mttr-watcher --since-time="${since}")"

PROBE_JSON="${probe_json}" WATCHER_LOGS="${watcher_logs}" VICTIM="${victim}" LIMIT_MS="${LIMIT_MS}" python3 - <<'PY'
import json, os, sys

limit = float(os.environ["LIMIT_MS"])
victim = os.environ["VICTIM"]
probe = json.loads(os.environ["PROBE_JSON"])

measured = {}
for line in os.environ["WATCHER_LOGS"].splitlines():
    try:
        entry = json.loads(line)
    except ValueError:
        continue
    if entry.get("message") == "eop-server failure measured" and entry.get("pod") == victim:
        measured[entry["kind"]] = float(entry["duration_ms"])

probe_ms = probe["longest_outage_ms"]
print(f"[mttr] probe: {probe['attempts']} requests, {probe['failures']} failed, "
      f"longest outage {probe_ms:.1f} ms")
restoration = measured.get("service_restoration")
replacement = measured.get("replica_replacement")
print(f"[mttr] mttr-watcher: service restoration "
      f"{'n/a' if restoration is None else f'{restoration:.1f} ms'}, replica replacement "
      f"{'n/a' if replacement is None else f'{replacement / 1000:.2f} s'}")

errors = []
if probe_ms >= limit:
    errors.append(f"the probe saw a {probe_ms:.1f} ms outage (limit {limit:.0f} ms)")
if restoration is None:
    errors.append("mttr-watcher did not measure the service restoration")
elif restoration >= limit:
    errors.append(f"service restoration took {restoration:.1f} ms (limit {limit:.0f} ms)")
if errors:
    sys.exit("[mttr] FAIL: " + "; ".join(errors))
print(f"[mttr] PASS: MTTR below {limit:.0f} ms")
PY
