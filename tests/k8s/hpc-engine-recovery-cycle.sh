#!/usr/bin/env bash
set -euo pipefail

# Self-healing check for hpc-engine (US-109 equivalent for the engine). Since
# ADR-027 the engine is a sidecar of every eop-server Pod: this deletes one
# server Pod, waits for the Deployment to replace it, and asserts the new Pod's
# engine container re-registered (as hpc-engine-<pod>).

NAMESPACE="${EOP_K8S_NAMESPACE:-default}"
DEPLOYMENT="${HPC_K8S_DEPLOYMENT:-eop-server}"
APP_LABEL="${HPC_K8S_APP_LABEL:-app=eop-server}"
ENGINE_CONTAINER="${HPC_K8S_CONTAINER:-hpc-engine}"

ROLLOUT_TIMEOUT_SECONDS="${HPC_ROLLOUT_TIMEOUT_SECONDS:-60}"

log() {
    printf '[hpc-recovery] %s\n' "$*" >&2
}

fail() {
    printf '[hpc-recovery] ERROR: %s\n' "$*" >&2
    exit 1
}

kubectl_ns() {
    kubectl -n "${NAMESPACE}" "$@"
}

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        fail "$1 is required but was not found in PATH."
    fi
}

wait_for_ready_pod() {
    log "Waiting for every ${APP_LABEL} Pod to become Ready..."
    kubectl_ns wait --for=condition=Ready pod -l "${APP_LABEL}" --timeout="${ROLLOUT_TIMEOUT_SECONDS}s" >/dev/null

    kubectl_ns get pods \
        -l "${APP_LABEL}" \
        --sort-by=.metadata.creationTimestamp \
        -o jsonpath='{range .items[*]}{.metadata.name}{"\n"}{end}' \
        | tail -n 1
}

assert_pod_logs_show_registration() {
    local pod="$1"

    log "Checking logs of Pod ${pod} for registration confirmation..."

    local logs
    logs="$(kubectl_ns logs "${pod}" -c "${ENGINE_CONTAINER}" 2>/dev/null || true)"

    if ! grep -q "HPC Engine registered as node_id=" <<<"${logs}"; then
        printf '%s\n' "${logs}" >&2
        fail "Pod ${pod} logs do not show a successful hpc-engine registration."
    fi

    log "Pod ${pod} confirmed registration."
}

main() {
    require_command kubectl

    log "Applying Kubernetes manifests from k8s/..."
    kubectl apply -f k8s/ >/dev/null

    log "Waiting for deployment/${DEPLOYMENT} rollout..."
    kubectl_ns rollout status "deployment/${DEPLOYMENT}" --timeout="${ROLLOUT_TIMEOUT_SECONDS}s"

    local pod_before
    pod_before="$(wait_for_ready_pod)"
    log "Ready Pod before deletion: ${pod_before}"
    assert_pod_logs_show_registration "${pod_before}"

    log "Deleting Pod ${pod_before} to validate Deployment-managed replacement..."
    kubectl_ns delete pod "${pod_before}" --wait=false >/dev/null
    kubectl_ns wait --for=delete "pod/${pod_before}" --timeout="${ROLLOUT_TIMEOUT_SECONDS}s" >/dev/null
    # With two replicas the other one is already Ready: wait for the Deployment to be complete
    # again, so the newest Pod below is the replacement.
    kubectl_ns rollout status "deployment/${DEPLOYMENT}" --timeout="${ROLLOUT_TIMEOUT_SECONDS}s" >/dev/null

    local pod_after
    pod_after="$(wait_for_ready_pod)"
    log "Replacement Ready Pod: ${pod_after}"

    [[ "${pod_after}" != "${pod_before}" ]] || fail "Expected a replacement Pod with a different name."

    assert_pod_logs_show_registration "${pod_after}"

    log "hpc-engine recovery cycle passed."
}

main "$@"
