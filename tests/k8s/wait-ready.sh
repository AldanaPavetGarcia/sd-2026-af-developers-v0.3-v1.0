#!/usr/bin/env bash
set -euo pipefail

# Waits until every Deployment, StatefulSet and DaemonSet in the namespace is rolled out and every
# Job has completed (make k8s-prod-apply). EOP_K8S_NAMESPACE (default), EOP_WAIT_TIMEOUT (300s).

NAMESPACE="${EOP_K8S_NAMESPACE:-default}"
TIMEOUT="${EOP_WAIT_TIMEOUT:-300s}"
k() { kubectl -n "${NAMESPACE}" "$@"; }

for kind in deployment statefulset daemonset; do
    for name in $(k get "${kind}" -o jsonpath='{.items[*].metadata.name}'); do
        k rollout status "${kind}/${name}" --timeout="${TIMEOUT}"
    done
done
for job in $(k get jobs -o jsonpath='{.items[*].metadata.name}'); do
    k wait --for=condition=complete "job/${job}" --timeout="${TIMEOUT}"
done
