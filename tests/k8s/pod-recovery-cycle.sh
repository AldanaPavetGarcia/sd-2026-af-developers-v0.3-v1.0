#!/usr/bin/env bash
set -euo pipefail

NAMESPACE="${EOP_K8S_NAMESPACE:-default}"
DEPLOYMENT="${EOP_K8S_DEPLOYMENT:-eop-server}"
SERVICE="${EOP_K8S_SERVICE:-eop-server}"
APP_LABEL="${EOP_K8S_APP_LABEL:-app=eop-server}"
CONTAINER_NAME="${EOP_K8S_CONTAINER:-eop-server}"

LOCAL_PORT="${EOP_LOCAL_PORT:-9026}"
REMOTE_PORT="${EOP_REMOTE_PORT:-9026}"

ROLLOUT_TIMEOUT_SECONDS="${EOP_ROLLOUT_TIMEOUT_SECONDS:-60}"
RECOVERY_TIMEOUT_SECONDS="${EOP_RECOVERY_TIMEOUT_SECONDS:-10}"
PORT_FORWARD_TIMEOUT_SECONDS="${EOP_PORT_FORWARD_TIMEOUT_SECONDS:-15}"
ACTIVE_CLIENT_TIMEOUT_SECONDS="${EOP_ACTIVE_CLIENT_TIMEOUT_SECONDS:-8}"

PORT_FORWARD_LOG="$(mktemp)"
ACTIVE_CLIENT_RESULT="$(mktemp)"
PORT_FORWARD_PID=""
ACTIVE_CLIENT_PID=""

log() {
    printf '[US-109] %s\n' "$*" >&2
}

fail() {
    printf '[US-109] ERROR: %s\n' "$*" >&2
    exit 1
}

kubectl_ns() {
    kubectl -n "${NAMESPACE}" "$@"
}

now_ms() {
    python3 -c 'import time; print(int(time.time() * 1000))'
}

cleanup() {
    if [[ -n "${PORT_FORWARD_PID}" ]] && kill -0 "${PORT_FORWARD_PID}" 2>/dev/null; then
        kill "${PORT_FORWARD_PID}" 2>/dev/null || true
        wait "${PORT_FORWARD_PID}" 2>/dev/null || true
    fi

    if [[ -n "${ACTIVE_CLIENT_PID}" ]] && kill -0 "${ACTIVE_CLIENT_PID}" 2>/dev/null; then
        kill "${ACTIVE_CLIENT_PID}" 2>/dev/null || true
        wait "${ACTIVE_CLIENT_PID}" 2>/dev/null || true
    fi

    rm -f "${PORT_FORWARD_LOG}" "${ACTIVE_CLIENT_RESULT}"
}

trap cleanup EXIT

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        fail "$1 is required but was not found in PATH."
    fi
}

assert_manifest_configuration() {
    log "Validating Deployment restart policy and probes..."

    local restart_policy
    local liveness_port
    local liveness_period
    local liveness_failure_threshold
    local readiness_port

    restart_policy="$(kubectl_ns get deployment "${DEPLOYMENT}" -o jsonpath='{.spec.template.spec.restartPolicy}')"
    liveness_port="$(kubectl_ns get deployment "${DEPLOYMENT}" -o jsonpath='{.spec.template.spec.containers[0].livenessProbe.tcpSocket.port}')"
    liveness_period="$(kubectl_ns get deployment "${DEPLOYMENT}" -o jsonpath='{.spec.template.spec.containers[0].livenessProbe.periodSeconds}')"
    liveness_failure_threshold="$(kubectl_ns get deployment "${DEPLOYMENT}" -o jsonpath='{.spec.template.spec.containers[0].livenessProbe.failureThreshold}')"
    readiness_port="$(kubectl_ns get deployment "${DEPLOYMENT}" -o jsonpath='{.spec.template.spec.containers[0].readinessProbe.tcpSocket.port}')"

    [[ "${restart_policy}" == "Always" ]] || fail "Expected restartPolicy=Always, got '${restart_policy}'."
    [[ "${liveness_port}" == "tcp-eop" || "${liveness_port}" == "${REMOTE_PORT}" ]] || fail "Unexpected liveness TCP port '${liveness_port}'."
    [[ "${liveness_period}" == "10" ]] || fail "Expected liveness periodSeconds=10, got '${liveness_period}'."
    [[ "${liveness_failure_threshold}" == "3" ]] || fail "Expected liveness failureThreshold=3, got '${liveness_failure_threshold}'."
    [[ "${readiness_port}" == "tcp-eop" || "${readiness_port}" == "${REMOTE_PORT}" ]] || fail "Unexpected readiness TCP port '${readiness_port}'."

    log "Deployment configuration is valid."
}

wait_for_ready_pod() {
    log "Waiting for an EOP server Pod to become Ready..."
    kubectl_ns wait --for=condition=Ready pod -l "${APP_LABEL}" --timeout="${ROLLOUT_TIMEOUT_SECONDS}s" >/dev/null

    kubectl_ns get pods \
        -l "${APP_LABEL}" \
        --sort-by=.metadata.creationTimestamp \
        -o jsonpath='{range .items[*]}{.metadata.name}{"\n"}{end}' \
        | tail -n 1
}

get_restart_count() {
    local pod="$1"

    kubectl_ns get pod "${pod}" \
        -o jsonpath="{.status.containerStatuses[?(@.name==\"${CONTAINER_NAME}\")].restartCount}" \
        2>/dev/null || true
}

is_pod_ready() {
    local pod="$1"

    kubectl_ns get pod "${pod}" \
        -o jsonpath='{.status.conditions[?(@.type=="Ready")].status}' \
        2>/dev/null || true
}

stop_port_forward() {
    if [[ -n "${PORT_FORWARD_PID}" ]] && kill -0 "${PORT_FORWARD_PID}" 2>/dev/null; then
        kill "${PORT_FORWARD_PID}" 2>/dev/null || true
        wait "${PORT_FORWARD_PID}" 2>/dev/null || true
    fi

    PORT_FORWARD_PID=""
}

start_port_forward() {
    stop_port_forward
    : > "${PORT_FORWARD_LOG}"

    log "Starting port-forward service/${SERVICE} ${LOCAL_PORT}:${REMOTE_PORT}..."
    kubectl_ns port-forward "service/${SERVICE}" "${LOCAL_PORT}:${REMOTE_PORT}" >"${PORT_FORWARD_LOG}" 2>&1 &
    PORT_FORWARD_PID="$!"

    local deadline=$((SECONDS + PORT_FORWARD_TIMEOUT_SECONDS))

    while (( SECONDS < deadline )); do
        if ! kill -0 "${PORT_FORWARD_PID}" 2>/dev/null; then
            cat "${PORT_FORWARD_LOG}" >&2 || true
            fail "kubectl port-forward exited before the server became reachable."
        fi

        if python3 - "${LOCAL_PORT}" <<'PY' >/dev/null 2>&1
import socket
import sys

port = int(sys.argv[1])

with socket.create_connection(("127.0.0.1", port), timeout=1):
    pass
PY
        then
            log "Port-forward is accepting TCP connections."
            return 0
        fi

        sleep 0.5
    done

    cat "${PORT_FORWARD_LOG}" >&2 || true
    fail "Timed out waiting for port-forward to accept connections."
}

run_register_smoke() {
    local node_id="$1"
    local attempts="${EOP_REGISTER_ATTEMPTS:-5}"
    local delay_seconds="${EOP_REGISTER_RETRY_DELAY_SECONDS:-1}"

    for attempt in $(seq 1 "${attempts}"); do
        log "Running REGISTER smoke attempt ${attempt}/${attempts} for node_id=${node_id}..."

        if EOP_HOST="127.0.0.1" \
            EOP_PORT="${LOCAL_PORT}" \
            EOP_NODE_ID="${node_id}" \
            ./tests/smoke/eop-register-smoke.sh; then
            return 0
        fi

        if (( attempt < attempts )); then
            log "REGISTER smoke failed; retrying in ${delay_seconds}s..."
            sleep "${delay_seconds}"
        fi
    done

    fail "REGISTER smoke failed after ${attempts} attempts for node_id=${node_id}."
}

start_active_client() {
    : > "${ACTIVE_CLIENT_RESULT}"

    log "Starting an active client that should observe a clean disconnection on crash..."

    python3 - "${LOCAL_PORT}" "${ACTIVE_CLIENT_RESULT}" "${ACTIVE_CLIENT_TIMEOUT_SECONDS}" <<'PY' &
import socket
import sys

port = int(sys.argv[1])
result_path = sys.argv[2]
disconnect_timeout = float(sys.argv[3])

try:
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        # Consume the welcome line if the production server sends it.
        sock.settimeout(0.5)
        try:
            sock.recv(4096)
        except socket.timeout:
            pass

        # After the simulated crash, recv() must return EOF or an error.
        sock.settimeout(disconnect_timeout)
        try:
            data = sock.recv(4096)
            if data == b"":
                outcome = "DISCONNECTED_EOF"
            else:
                outcome = "UNEXPECTED_DATA"
        except socket.timeout:
            outcome = "TIMEOUT"
        except (ConnectionResetError, BrokenPipeError, OSError) as exc:
            outcome = f"DISCONNECTED_ERROR:{type(exc).__name__}"

    with open(result_path, "w", encoding="utf-8") as result:
        result.write(outcome + "\n")

except Exception as exc:
    with open(result_path, "w", encoding="utf-8") as result:
        result.write(f"CLIENT_ERROR:{type(exc).__name__}:{exc}\n")
PY

    ACTIVE_CLIENT_PID="$!"
    sleep 1
}

assert_active_client_disconnected() {
    log "Waiting for the active client to observe the disconnection..."

    local deadline=$((SECONDS + ACTIVE_CLIENT_TIMEOUT_SECONDS + 2))

    while (( SECONDS < deadline )); do
        if ! kill -0 "${ACTIVE_CLIENT_PID}" 2>/dev/null; then
            wait "${ACTIVE_CLIENT_PID}" 2>/dev/null || true
            break
        fi

        sleep 0.2
    done

    if kill -0 "${ACTIVE_CLIENT_PID}" 2>/dev/null; then
        kill "${ACTIVE_CLIENT_PID}" 2>/dev/null || true
        wait "${ACTIVE_CLIENT_PID}" 2>/dev/null || true
        fail "Active client did not observe disconnection before timeout."
    fi

    local outcome
    outcome="$(cat "${ACTIVE_CLIENT_RESULT}")"

    case "${outcome}" in
        DISCONNECTED_EOF|DISCONNECTED_ERROR:*)
            log "Active client observed disconnection error: ${outcome}"
            ;;
        TIMEOUT)
            log "Active client observed bounded timeout after server termination: ${outcome}"
            ;;
        *)
            fail "Active client did not receive the expected bounded connection failure. Outcome: ${outcome}"
            ;;
    esac    
}

wait_for_container_restart_ready() {
    local pod="$1"
    local previous_restart_count="$2"

    local start
    start="$(now_ms)"

    local deadline=$((SECONDS + RECOVERY_TIMEOUT_SECONDS))

    while (( SECONDS < deadline )); do
        local current_restart_count
        local ready

        current_restart_count="$(get_restart_count "${pod}")"
        ready="$(is_pod_ready "${pod}")"

        if [[ -n "${current_restart_count}" ]] \
            && (( current_restart_count > previous_restart_count )) \
            && [[ "${ready}" == "True" ]]; then
            local end
            local elapsed_ms

            end="$(now_ms)"
            elapsed_ms=$((end - start))

            log "Container restarted and Pod is Ready in ${elapsed_ms} ms."

            if (( elapsed_ms >= RECOVERY_TIMEOUT_SECONDS * 1000 )); then
                fail "Recovery took ${elapsed_ms} ms, expected less than $((RECOVERY_TIMEOUT_SECONDS * 1000)) ms."
            fi

            return 0
        fi

        sleep 0.25
    done

    fail "Container did not restart and return to Ready within ${RECOVERY_TIMEOUT_SECONDS}s."
}

assert_previous_logs_include_crash_reason() {
    local pod="$1"

    log "Checking kubectl logs and kubectl logs --previous..."

    kubectl_ns logs "${pod}" --tail=20 >/dev/null

    local previous_logs
    previous_logs="$(kubectl_ns logs "${pod}" --previous 2>/dev/null || true)"

    if ! grep -q "US-109 simulated crash reason" <<<"${previous_logs}"; then
        printf '%s\n' "${previous_logs}" >&2
        fail "kubectl logs --previous did not include the simulated crash reason."
    fi

    log "Previous container logs include the simulated crash reason."
}

simulate_server_crash() {
    local pod="$1"

    log "Simulating server process termination in Pod ${pod}..."

    kubectl_ns exec "${pod}" -- sh -c '
        set -eu

        echo "US-109 simulated crash reason: forced server process termination" >/proc/1/fd/1
        echo "Process table before termination:" >/proc/1/fd/1
        ps >/proc/1/fd/1

        kill -TERM 1
    ' || true
}

delete_pod_and_wait_for_replacement() {
    local pod="$1"

    log "Deleting Pod ${pod} to validate Deployment-managed replacement..."
    kubectl_ns delete pod "${pod}" --wait=false >/dev/null
    kubectl_ns wait --for=delete "pod/${pod}" --timeout="${ROLLOUT_TIMEOUT_SECONDS}s" >/dev/null

    wait_for_ready_pod
}

reset_pods_for_clean_recovery_measurement() {
    log "Resetting existing Pods to start recovery measurement from a clean state..."

    kubectl_ns delete pod -l "${APP_LABEL}" --ignore-not-found=true >/dev/null

    kubectl_ns wait \
        --for=condition=Ready \
        pod \
        -l "${APP_LABEL}" \
        --timeout="${ROLLOUT_TIMEOUT_SECONDS}s" >/dev/null
}

main() {
    require_command kubectl
    require_command python3

    [[ -x ./tests/smoke/eop-register-smoke.sh ]] || fail "./tests/smoke/eop-register-smoke.sh is required and must be executable."

    log "Applying Kubernetes manifests from k8s/..."
    kubectl apply -f k8s/ >/dev/null

    log "Waiting for deployment/${DEPLOYMENT} rollout..."
    kubectl_ns rollout status "deployment/${DEPLOYMENT}" --timeout="${ROLLOUT_TIMEOUT_SECONDS}s"

    assert_manifest_configuration
    reset_pods_for_clean_recovery_measurement

    local pod
    pod="$(wait_for_ready_pod)"
    log "Ready Pod: ${pod}"

    start_port_forward

    run_register_smoke "us109-before-crash-$(date +%s)"

    local restart_count_before
    restart_count_before="$(get_restart_count "${pod}")"
    [[ -n "${restart_count_before}" ]] || fail "Could not read restart count for Pod ${pod}."

    simulate_server_crash "${pod}"

    wait_for_container_restart_ready "${pod}" "${restart_count_before}"
    assert_previous_logs_include_crash_reason "${pod}"

    stop_port_forward
    start_port_forward

    run_register_smoke "us109-after-crash-$(date +%s)"

    start_active_client
    simulate_server_crash "${pod}"
    assert_active_client_disconnected

    stop_port_forward

    local pod_before_delete
    local pod_after_delete

    pod_before_delete="$(wait_for_ready_pod)"
    pod_after_delete="$(delete_pod_and_wait_for_replacement "${pod_before_delete}")"

    log "Replacement Ready Pod: ${pod_after_delete}"

    stop_port_forward
    start_port_forward

    run_register_smoke "us109-after-delete-$(date +%s)"

    log "US-109 pod recovery cycle passed."
}

main "$@"