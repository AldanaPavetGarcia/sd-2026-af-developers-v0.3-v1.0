#!/usr/bin/env bash
# Live demo of EOP v1.0 to Vault-Tec (US-407, #75), against the stack of deploy/production in the
# current kubectl context. Four steps, each one explained, run and checked; the script stops at the
# first failure with what failed, and always leaves the cluster healthy (canary weight 0, demo
# Pods removed). Safe to run again.
#
#   1  kubectl delete pod of an eop-server replica: MTTR from the probe and mttr-watcher (< 200 ms)
#   2  canary of the API gateway at 10 → 50 → 100 %, with the per-version error rate and P99
#   3  one edge message end to end: device → Mosquitto → ingestor → eop-server → /v1/nodes, and its
#      trace in SigNoz
#   4  make smoke-test-prod
#
#   DEMO_AUTO=1            no pauses (rehearsal, CI)
#   DEMO_STEPS="1 3"       run only these steps
#   DEMO_CANARY_IMAGE      canary image (default api-gateway:local); DEMO_CANARY_VERSION (1.0.1)
#   DEMO_EDGE_SIM_IMAGE    simulated device image (default edge-sim:local)
#   DEMO_CANARY_SECONDS    seconds of load at each canary weight (default 20)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NAMESPACE="${EOP_K8S_NAMESPACE:-default}"
STEPS="${DEMO_STEPS:-1 2 3 4}"
CANARY_IMAGE="${DEMO_CANARY_IMAGE:-api-gateway:local}"
CANARY_VERSION="${DEMO_CANARY_VERSION:-1.0.1}"
EDGE_SIM_IMAGE="${DEMO_EDGE_SIM_IMAGE:-edge-sim:local}"
CANARY_SECONDS="${DEMO_CANARY_SECONDS:-20}"
CANARY_WEIGHTS=(10 50 100)
EDGE_SAMPLES=10
EDGE_TIMEOUT_S=60
GATEWAY_LOCAL_PORT="${DEMO_GATEWAY_PORT:-18080}"
LOAD_LOG="$(mktemp -t eop-demo-load.XXXXXX)"
EDGE_POD=""
CANARY_STARTED=0
PIDS=()

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
note() { printf '   %s\n' "$*"; }
die() { printf '\n\033[31mDEMO FAILED: %s\033[0m\n' "$*" >&2; exit 1; }
k() { kubectl -n "$NAMESPACE" "$@"; }

pause() {
    if [[ "${DEMO_AUTO:-0}" == "1" ]]; then return; fi
    read -r -p "   [Enter] $1 " _ < /dev/tty
}

cleanup() {
    local status=$?
    for pid in "${PIDS[@]}"; do kill "$pid" 2> /dev/null || true; done
    if [[ -n "$EDGE_POD" ]]; then k delete pod "$EDGE_POD" --ignore-not-found --wait=false > /dev/null 2>&1 || true; fi
    if (( status != 0 && CANARY_STARTED )); then
        note "leaving the cluster healthy: canary weight back to 0"
        "$ROOT/scripts/canary.sh" rollback > /dev/null 2>&1 || true
    fi
    rm -f "$LOAD_LOG"
}
trap cleanup EXIT

wanted() { [[ " $STEPS " == *" $1 "* ]]; }

ready() {
    local name="$1" want="$2" got
    got="$(k get deployment "$name" -o jsonpath='{.status.readyReplicas}' 2> /dev/null || true)"
    [[ "${got:-0}" -ge "$want" ]] || die "deployment/$name has ${got:-0} ready replicas, needs $want (run make k8s-prod-apply)"
}

preflight() {
    say "Preflight"
    for tool in kubectl python3 make; do
        command -v "$tool" > /dev/null || die "$tool is required"
    done
    local context
    context="$(kubectl config current-context 2> /dev/null)" || die "no kubectl context (is the cluster running?)"
    note "context: $context, namespace: $NAMESPACE"
    ready eop-server 2
    ready mttr-watcher 1
    ready api-gateway-stable 1
    ready edge-ingestor 1
    k get statefulset mosquitto > /dev/null 2>&1 || k get deployment mosquitto > /dev/null 2>&1 \
        || die "Mosquitto is not deployed"
    note "eop-server x2, mttr-watcher, api-gateway, edge-ingestor and Mosquitto are Ready"
    note "Grafana: Vault-Tec Delivery — EOP v1.0 (kubectl port-forward svc/grafana 3000)"
}

step_mttr() {
    say "1. A replica dies: MTTR (US-407 AC4)"
    note "Two eop-server replicas share the node registry in Redis (ADR-027). We delete one Pod while"
    note "a probe sends LIST_NODES through the Service every 20 ms; mttr-watcher times the failover."
    k get pods -l app=eop-server -o wide
    pause "to delete a Pod"
    "$ROOT/tests/k8s/mttr-cycle.sh" || die "MTTR at or above 200 ms (see the lines above)"
    k get pods -l app=eop-server
    note "Grafana → Failover row: 'MTTR on Pod Failure' and, next to it, the replica replacement time."
}

step_canary() {
    say "2. Canary release of the API gateway (US-304, US-407)"
    note "The new version gets 10 %, then 50 %, then 100 % of the traffic while clients keep calling."
    pause "to deploy $CANARY_IMAGE as version $CANARY_VERSION on the canary track"
    CANARY_STARTED=1
    "$ROOT/scripts/canary.sh" deploy "$CANARY_IMAGE" "$CANARY_VERSION" || die "canary deploy failed"
    python3 "$ROOT/tests/load/gateway_load.py" --report-every 10 > "$LOAD_LOG" 2>&1 &
    PIDS+=("$!")
    local load_pid=$!
    for weight in "${CANARY_WEIGHTS[@]}"; do
        "$ROOT/scripts/canary.sh" rollout "$weight" || die "could not set the canary weight to $weight"
        note "weight $weight %: Grafana → Canary row (requests, error rate and P99 by version)"
        sleep "$CANARY_SECONDS"
        tail -n 4 "$LOAD_LOG" | sed 's/^/   | /'
        kill -0 "$load_pid" 2> /dev/null || die "the client load stopped: $(tail -n 5 "$LOAD_LOG")"
        pause "for the next weight"
    done
    "$ROOT/scripts/canary.sh" promote || die "canary promote failed"
    kill -TERM "$load_pid" 2> /dev/null || true
    local status=0
    wait "$load_pid" || status=$?
    tail -n 12 "$LOAD_LOG" | sed 's/^/   | /'
    (( status == 0 )) || die "clients saw 5xx or connection errors during the rollout"
    CANARY_STARTED=0
    note "Promoted: stable runs $CANARY_VERSION and no client saw an error."
}

gateway_get() {
    python3 - "$1" << 'PY'
import json, sys, urllib.request
try:
    with urllib.request.urlopen(sys.argv[1], timeout=5) as r:
        print(json.dumps(json.load(r).get("data", {})))
except Exception:
    print("{}")
PY
}

step_edge() {
    say "3. One edge message end to end (US-404, US-405)"
    note "A simulated bunker publishes $EDGE_SAMPLES samples to Mosquitto; the ingestor forwards them"
    note "to eop-server, and the gateway shows the device with its telemetry."
    local prefix node
    prefix="demo-$(date +%H%M%S)-"
    node="${prefix}1"
    EDGE_POD="${prefix%-}"
    pause "to start the device $node"
    k run "$EDGE_POD" --image="$EDGE_SIM_IMAGE" --image-pull-policy=IfNotPresent --restart=Never \
        --env=EOP_MQTT_BROKER=tcp://mosquitto:1883 --env=EOP_SIM_DEVICES=1 \
        --env="EOP_SIM_COUNT=$EDGE_SAMPLES" --env="EOP_SIM_NODE_PREFIX=$prefix" > /dev/null \
        || die "could not start the simulated device"
    k port-forward svc/api-gateway "$GATEWAY_LOCAL_PORT:80" > /dev/null 2>&1 &
    PIDS+=("$!")
    local deadline=$((SECONDS + EDGE_TIMEOUT_S)) data="{}"
    while (( SECONDS < deadline )); do
        data="$(gateway_get "http://127.0.0.1:$GATEWAY_LOCAL_PORT/v1/nodes/$node")"
        if [[ "$data" == *'"telemetry"'* ]]; then break; fi
        sleep 1
    done
    [[ "$data" == *'"telemetry"'* ]] || die "$node did not reach /v1/nodes within ${EDGE_TIMEOUT_S} s"
    note "GET /v1/nodes/$node →"
    python3 -m json.tool <<< "$data" | sed 's/^/   | /'
    note "GET /v1/edge/status →"
    gateway_get "http://127.0.0.1:$GATEWAY_LOCAL_PORT/v1/edge/status" | python3 -m json.tool | head -12 | sed 's/^/   | /'
    note "SigNoz (kubectl port-forward svc/signoz 8080): Traces → service edge-ingestor, operation"
    note "edge.message, attribute edge.node_id=$node: sampled → received by the ingestor → eop-server span."
    pause "to continue"
}

step_smoke() {
    say "4. Smoke test of the product (US-406)"
    make -C "$ROOT" smoke-test-prod || die "smoke-test-prod failed"
    note "Server, registration, graph analysis, edge ingestion and gateway health: all green."
}

preflight
wanted 1 && step_mttr
wanted 2 && step_canary
wanted 3 && step_edge
wanted 4 && step_smoke
say "Demo complete: the cluster is healthy."
