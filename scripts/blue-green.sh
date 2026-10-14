#!/usr/bin/env bash
# Blue-green delivery of the api-gateway (US-305, ADR-011 §3). Called by the make targets:
#
#   blue-green.sh deploy IMAGE VERSION   the idle slot runs IMAGE, labelled VERSION, 1 replica
#   blue-green.sh switch                 smoke tests on the idle slot; only if they pass, the
#                                        active Service selector moves to it (one patch)
#   blue-green.sh rollback               selector back to the other slot, no smoke tests (it was
#                                        serving moments ago); timed, limit 10 s
#   blue-green.sh status                 active slot and image, version, ready pods per slot
#   blue-green.sh layout canary|blue-green
#                                        moves the gateway between the two layouts of ADR-011 §4
#                                        (not zero-downtime: do it before the demo's load test)
set -euo pipefail
export LC_NUMERIC=C  # decimal point in printf/bc whatever the operator's locale

ACTIVE=api-gateway-active
CONTAINER=api-gateway
ROLLOUT_TIMEOUT=180s
SWITCH_LIMIT_SECONDS=10
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

die() { echo "blue-green: $*" >&2; exit 1; }

active_slot() {
    kubectl get service "$ACTIVE" -o jsonpath='{.spec.selector.slot}'
}

other_slot() {
    [[ "$1" == blue ]] && echo green || echo blue
}

ready_replicas() {
    local ready
    ready=$(kubectl get deployment "api-gateway-$1" -o jsonpath='{.status.readyReplicas}')
    echo "${ready:-0}"
}

slot_version() {
    kubectl get deployment "api-gateway-$1" -o jsonpath='{.spec.template.metadata.labels.version}'
}

set_active() {
    kubectl patch service "$ACTIVE" --type merge -p "{\"spec\": {\"selector\": {\"slot\": \"$1\"}}}" > /dev/null
}

# Moves the active Service to $1 and waits, through the Ingress, until no client is served by
# $2 any more. The time printed is what clients experience, from the start of the switch.
switch_to() {
    local target="$1" previous="$2" started
    started=$(date +%s.%N)
    set_active "$target"
    printf 'blue-green: %s selector -> slot=%s in %.2f s\n' "$ACTIVE" "$target" "$(echo "$(date +%s.%N) - $started" | bc)"
    python3 "$ROOT/tests/load/gateway_load.py" --until-absent "$previous" --since "$started" --timeout "$SWITCH_LIMIT_SECONDS"
}

cmd_deploy() {
    local image="${1:-}" version="${2:-}" idle
    [[ -n "$image" && -n "$version" ]] || die "usage: deploy IMAGE VERSION"
    idle=$(other_slot "$(active_slot)")
    kubectl patch deployment "api-gateway-$idle" --type strategic -p "$(cat <<EOF
{"spec": {"replicas": 1, "template": {"metadata": {"labels": {"version": "$version"}},
 "spec": {"containers": [{"name": "$CONTAINER", "image": "$image"}]}}}}
EOF
)"
    kubectl rollout status "deployment/api-gateway-$idle" --timeout="$ROLLOUT_TIMEOUT"
    echo "blue-green: $idle runs $image ($version); $(active_slot) is still active"
}

cmd_switch() {
    local current target version port pf rc=0
    current=$(active_slot)
    target=$(other_slot "$current")
    (( $(ready_replicas "$target") > 0 )) || die "$target has no ready pod; run make blue-green-deploy first. $current stays active"
    version=$(slot_version "$target")

    # Smoke tests reach the target slot only, through its preview Service (US-305 AC3).
    port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
    kubectl port-forward "service/api-gateway-$target-preview" "$port:80" > /dev/null 2>&1 &
    pf=$!
    trap 'kill $pf 2>/dev/null || true' EXIT
    for _ in $(seq 1 50); do
        (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null && break
        sleep 0.2
    done
    echo "blue-green: smoke tests on $target ($version)"
    python3 "$ROOT/tests/smoke/gateway_smoke.py" "http://127.0.0.1:$port" --expect-version "$version" || rc=$?
    kill "$pf" 2>/dev/null || true
    trap - EXIT
    (( rc == 0 )) || die "smoke tests failed on $target: no switch, $current stays active"

    switch_to "$target" "$current"
    echo "blue-green: $target ($version) is active; $current keeps running for a rollback"
}

cmd_rollback() {
    local current target
    current=$(active_slot)
    target=$(other_slot "$current")
    (( $(ready_replicas "$target") > 0 )) || die "$target has no ready pod to roll back to"
    switch_to "$target" "$current"
    echo "blue-green: rolled back to $target ($(slot_version "$target"))"
}

cmd_status() {
    echo "active: $(active_slot)"
    for slot in blue green; do
        kubectl get deployment "api-gateway-$slot" -o jsonpath="{.metadata.name}: {.spec.template.spec.containers[0].image} version={.spec.template.metadata.labels.version} ready={.status.readyReplicas}/{.spec.replicas}{'\n'}"
    done
}

# Image and version label of a Deployment, as a strategic patch that hands them to another one.
image_patch() {
    local image version
    image=$(kubectl get deployment "$1" -o jsonpath="{.spec.template.spec.containers[?(@.name=='$CONTAINER')].image}")
    version=$(kubectl get deployment "$1" -o jsonpath='{.spec.template.metadata.labels.version}')
    echo "{\"spec\": {\"template\": {\"metadata\": {\"labels\": {\"version\": \"$version\"}}, \"spec\": {\"containers\": [{\"name\": \"$CONTAINER\", \"image\": \"$image\"}]}}}}"
}

# The version being served moves to the new layout; then the old layout's objects are removed.
cmd_layout() {
    local carry
    case "${1:-}" in
        blue-green)
            carry=$(image_patch api-gateway-stable)
            kubectl apply -k "$ROOT/k8s/api-gateway/overlays/blue-green"
            kubectl patch deployment api-gateway-blue --type strategic -p "$carry" > /dev/null
            kubectl rollout status deployment/api-gateway-blue --timeout="$ROLLOUT_TIMEOUT"
            kubectl delete ingress api-gateway-canary --ignore-not-found
            kubectl delete service api-gateway api-gateway-stable api-gateway-canary --ignore-not-found
            kubectl delete deployment api-gateway-stable api-gateway-canary --ignore-not-found
            ;;
        canary)
            carry=$(image_patch "api-gateway-$(active_slot)")
            kubectl apply -k "$ROOT/k8s/api-gateway/overlays/canary"
            kubectl patch deployment api-gateway-stable --type strategic -p "$carry" > /dev/null
            kubectl rollout status deployment/api-gateway-stable --timeout="$ROLLOUT_TIMEOUT"
            kubectl delete service "$ACTIVE" api-gateway-blue-preview api-gateway-green-preview --ignore-not-found
            kubectl delete deployment api-gateway-blue api-gateway-green --ignore-not-found
            ;;
        *) die "usage: layout canary|blue-green" ;;
    esac
}

case "${1:-}" in
    deploy) shift; cmd_deploy "$@" ;;
    switch) cmd_switch ;;
    rollback) cmd_rollback ;;
    status) cmd_status ;;
    layout) shift; cmd_layout "$@" ;;
    *) die "usage: $0 {deploy IMAGE VERSION|switch|rollback|status|layout canary|blue-green}" ;;
esac
