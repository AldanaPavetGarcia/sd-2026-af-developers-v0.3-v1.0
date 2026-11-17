#!/usr/bin/env bash
set -euo pipefail

# US-406 AC4: from `kubectl apply -k deploy/production` to every workload Ready in under 3 minutes,
# on a clean cluster with the images already present (make gateway-image + the eop-server and
# hpc-engine images, loaded into the cluster).
#
# It refuses to run where EOP is already deployed: measuring there would only time a no-op
# apply, and it must never touch a running deployment's data (the 30-day SLO run). Use a separate
# cluster, e.g. `minikube start -p eop-clean` and `minikube -p eop-clean image load …`.

NAMESPACE="${EOP_K8S_NAMESPACE:-default}"
LIMIT_SECONDS="${EOP_READY_LIMIT_SECONDS:-180}"
OVERLAY="${EOP_OVERLAY:-deploy/production}"

log() { printf '[ready-time] %s\n' "$*" >&2; }
fail() { printf '[ready-time] FAIL: %s\n' "$*" >&2; exit 1; }
k() { kubectl -n "${NAMESPACE}" "$@"; }

command -v kubectl >/dev/null || fail "kubectl is required"
if k get deployment eop-server >/dev/null 2>&1; then
    fail "EOP is already deployed in namespace ${NAMESPACE}; run this on a clean cluster (see the header)"
fi

start=$(date +%s)
log "Applying ${OVERLAY}..."
k apply -k "${OVERLAY}" >/dev/null
deadline=$((start + LIMIT_SECONDS))

remaining() {
    local left=$((deadline - $(date +%s)))
    ((left > 0)) || left=1
    echo "${left}s"
}

for kind in deployment statefulset daemonset; do
    for name in $(k get "${kind}" -o jsonpath='{.items[*].metadata.name}'); do
        k rollout status "${kind}/${name}" --timeout="$(remaining)" >/dev/null ||
            fail "${kind}/${name} not Ready within ${LIMIT_SECONDS} s"
    done
done
for job in $(k get jobs -o jsonpath='{.items[*].metadata.name}'); do
    k wait --for=condition=complete "job/${job}" --timeout="$(remaining)" >/dev/null ||
        fail "job/${job} not complete within ${LIMIT_SECONDS} s"
done

elapsed=$(($(date +%s) - start))
log "every workload Ready ${elapsed} s after the apply (limit ${LIMIT_SECONDS} s)"
((elapsed < LIMIT_SECONDS)) || fail "took ${elapsed} s"
log "PASS"
