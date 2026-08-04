#!/usr/bin/env bash
set -euo pipefail

# AC7 (US-204): a rollout restart of eop-server must complete with zero
# failed/dropped client operations. EOP has no HTTP/5xx concept, so "no 5xx"
# translates to "zero failed REGISTER round-trips observed by a continuously
# running client during the restart".

NAMESPACE="${EOP_K8S_NAMESPACE:-default}"
DEPLOYMENT="${EOP_K8S_DEPLOYMENT:-eop-server}"
SERVICE="${EOP_K8S_SERVICE:-eop-server}"

LOCAL_PORT="${EOP_LOCAL_PORT:-9026}"
REMOTE_PORT="${EOP_REMOTE_PORT:-9026}"

ROLLOUT_TIMEOUT_SECONDS="${EOP_ROLLOUT_TIMEOUT_SECONDS:-60}"
PORT_FORWARD_TIMEOUT_SECONDS="${EOP_PORT_FORWARD_TIMEOUT_SECONDS:-15}"
LOAD_CLIENT_WARMUP_SECONDS="${EOP_LOAD_CLIENT_WARMUP_SECONDS:-2}"
LOAD_CLIENT_COOLDOWN_SECONDS="${EOP_LOAD_CLIENT_COOLDOWN_SECONDS:-2}"

PORT_FORWARD_LOG="$(mktemp)"
LOAD_CLIENT_RESULT="$(mktemp)"
PORT_FORWARD_PID=""
LOAD_CLIENT_PID=""

log() {
    printf '[AC7] %s\n' "$*" >&2
}

fail() {
    printf '[AC7] ERROR: %s\n' "$*" >&2
    exit 1
}

kubectl_ns() {
    kubectl -n "${NAMESPACE}" "$@"
}

cleanup() {
    if [[ -n "${LOAD_CLIENT_PID}" ]] && kill -0 "${LOAD_CLIENT_PID}" 2>/dev/null; then
        kill "${LOAD_CLIENT_PID}" 2>/dev/null || true
        wait "${LOAD_CLIENT_PID}" 2>/dev/null || true
    fi

    if [[ -n "${PORT_FORWARD_PID}" ]] && kill -0 "${PORT_FORWARD_PID}" 2>/dev/null; then
        kill "${PORT_FORWARD_PID}" 2>/dev/null || true
        wait "${PORT_FORWARD_PID}" 2>/dev/null || true
    fi

    rm -f "${PORT_FORWARD_LOG}" "${LOAD_CLIENT_RESULT}"
}

trap cleanup EXIT

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        fail "$1 is required but was not found in PATH."
    fi
}

start_port_forward() {
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

# Continuously issues REGISTER round-trips against the server (one connection
# per iteration, mirroring eop-register-smoke.sh's protocol) until SIGTERM,
# and writes a final "operations=<n> failures=<n>" summary line on exit.
start_load_client() {
    : > "${LOAD_CLIENT_RESULT}"

    log "Starting continuous REGISTER load client..."

    python3 - "${LOCAL_PORT}" "${LOAD_CLIENT_RESULT}" <<'PY' &
import signal
import socket
import sys
import time

port = int(sys.argv[1])
result_path = sys.argv[2]

operations = 0
failures = 0
stopping = False


def handle_term(signum, frame):
    global stopping
    stopping = True


signal.signal(signal.SIGTERM, handle_term)

while not stopping:
    node_id = f"ac7-load-{operations}-{int(time.time() * 1000)}"
    register_command = (
        f"REGISTER node_id={node_id} bunker_name=AC7Load "
        f"ip_address=10.7.0.1 capacity=7\n"
    )

    operations += 1
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
            sock.settimeout(3)
            sock.sendall(register_command.encode("utf-8"))

            buffer = ""
            acked = False
            deadline = time.monotonic() + 3

            while time.monotonic() < deadline and not acked:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                buffer += chunk.decode("utf-8", errors="replace")
                for line in buffer.splitlines():
                    if line.startswith("ACK"):
                        acked = True
                        break

            if not acked:
                failures += 1
    except OSError:
        failures += 1

    time.sleep(0.05)

with open(result_path, "w", encoding="utf-8") as result:
    result.write(f"operations={operations} failures={failures}\n")
PY

    LOAD_CLIENT_PID="$!"
    sleep "${LOAD_CLIENT_WARMUP_SECONDS}"

    if ! kill -0 "${LOAD_CLIENT_PID}" 2>/dev/null; then
        fail "Load client exited immediately; check connectivity to service/${SERVICE}."
    fi
}

stop_load_client_and_assert_zero_failures() {
    log "Stopping load client and collecting results..."

    kill -TERM "${LOAD_CLIENT_PID}" 2>/dev/null || true
    wait "${LOAD_CLIENT_PID}" 2>/dev/null || true
    LOAD_CLIENT_PID=""

    local summary
    summary="$(cat "${LOAD_CLIENT_RESULT}")"
    log "Load client summary: ${summary}"

    [[ -n "${summary}" ]] || fail "Load client produced no result summary."

    local failures
    failures="$(grep -oP 'failures=\K[0-9]+' <<<"${summary}" || true)"

    [[ -n "${failures}" ]] || fail "Could not parse failure count from summary: ${summary}"
    [[ "${failures}" -eq 0 ]] || fail "Observed ${failures} failed client operations during rollout restart."
}

main() {
    require_command kubectl
    require_command python3

    log "Applying Kubernetes manifests from k8s/..."
    kubectl apply -f k8s/ >/dev/null

    log "Waiting for deployment/${DEPLOYMENT} rollout..."
    kubectl_ns rollout status "deployment/${DEPLOYMENT}" --timeout="${ROLLOUT_TIMEOUT_SECONDS}s"

    start_port_forward
    start_load_client

    log "Triggering rollout restart of deployment/${DEPLOYMENT} under load..."
    kubectl_ns rollout restart "deployment/${DEPLOYMENT}"
    kubectl_ns rollout status "deployment/${DEPLOYMENT}" --timeout="${ROLLOUT_TIMEOUT_SECONDS}s"

    log "Rollout complete; cooling down before stopping the load client..."
    sleep "${LOAD_CLIENT_COOLDOWN_SECONDS}"

    stop_load_client_and_assert_zero_failures

    log "AC7 rollout-restart-under-load passed."
}

main "$@"
