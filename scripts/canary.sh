#!/usr/bin/env bash
# Canary release of the api-gateway (US-304, ADR-011 §2). Called by the make targets:
#
#   canary.sh deploy IMAGE VERSION   canary track runs IMAGE, labelled VERSION, 1 replica (weight unchanged)
#   canary.sh rollout WEIGHT         percent of the Ingress traffic sent to the canary (0-100)
#   canary.sh rollback               weight 0, then measures until no response comes from the canary
#   canary.sh promote                stable takes the canary's image and version, then weight 0 and
#                                    the canary goes back to 0 replicas
#   canary.sh status                 weight, image, version and ready pods of each track
#
# The only object a weight change touches is the canary Ingress annotation (US-304 AC7).
set -euo pipefail
export LC_NUMERIC=C  # decimal point in printf/bc whatever the operator's locale

INGRESS=api-gateway-canary
WEIGHT_ANNOTATION=nginx.ingress.kubernetes.io/canary-weight
STABLE=api-gateway-stable
CANARY=api-gateway-canary
CONTAINER=api-gateway
ROLLOUT_TIMEOUT=180s
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

die() { echo "canary: $*" >&2; exit 1; }

ready_replicas() {
    local ready
    ready=$(kubectl get deployment "$1" -o jsonpath='{.status.readyReplicas}')
    echo "${ready:-0}"
}

current_weight() {
    kubectl get ingress "$INGRESS" -o jsonpath="{.metadata.annotations.${WEIGHT_ANNOTATION//./\\.}}"
}

set_weight() {
    kubectl annotate ingress "$INGRESS" --overwrite "$WEIGHT_ANNOTATION=$1" > /dev/null
}

cmd_deploy() {
    local image="${1:-}" version="${2:-}"
    [[ -n "$image" && -n "$version" ]] || die "usage: deploy IMAGE VERSION"
    # One patch, so the image and the version label change in a single rollout.
    kubectl patch deployment "$CANARY" --type strategic -p "$(cat <<EOF
{"spec": {"replicas": 1, "template": {"metadata": {"labels": {"version": "$version"}},
 "spec": {"containers": [{"name": "$CONTAINER", "image": "$image"}]}}}}
EOF
)"
    kubectl rollout status "deployment/$CANARY" --timeout="$ROLLOUT_TIMEOUT"
    echo "canary: $CANARY runs $image ($version); weight still $(current_weight) %"
}

cmd_rollout() {
    local weight="${1:-}"
    [[ "$weight" =~ ^[0-9]+$ ]] && (( weight <= 100 )) || die "WEIGHT must be an integer from 0 to 100, got '$weight'"
    # Sending traffic to a track with no ready pod would answer 503 to that share of clients.
    if (( weight > 0 )) && (( $(ready_replicas "$CANARY") == 0 )); then
        die "$CANARY has no ready pod; run make canary-deploy first"
    fi
    local before
    before="$(current_weight)"
    set_weight "$weight"
    echo "canary: weight ${before:-0} % -> $weight %"
}

cmd_rollback() {
    local started
    started=$(date +%s.%N)
    set_weight 0
    printf 'canary: weight set to 0 in %.2f s\n' "$(echo "$(date +%s.%N) - $started" | bc)"
    # The annotation takes effect when ingress-nginx syncs; US-304 AC4 bounds the time until
    # clients stop getting canary responses (< 30 s), measured from the start of the rollback.
    python3 "$ROOT/tests/load/gateway_load.py" --until-absent canary --since "$started" --timeout 30
}

cmd_promote() {
    local image version
    image=$(kubectl get deployment "$CANARY" -o jsonpath="{.spec.template.spec.containers[?(@.name=='$CONTAINER')].image}")
    version=$(kubectl get deployment "$CANARY" -o jsonpath='{.spec.template.metadata.labels.version}')
    (( $(ready_replicas "$CANARY") > 0 )) || die "$CANARY has no ready pod: nothing to promote"
    # Rolling update of stable behind its readiness probe (maxUnavailable 0) while the canary
    # still serves its share; only then does all traffic return to stable.
    kubectl patch deployment "$STABLE" --type strategic -p "$(cat <<EOF
{"spec": {"template": {"metadata": {"labels": {"version": "$version"}},
 "spec": {"containers": [{"name": "$CONTAINER", "image": "$image"}]}}}}
EOF
)"
    kubectl rollout status "deployment/$STABLE" --timeout="$ROLLOUT_TIMEOUT"
    set_weight 0
    python3 "$ROOT/tests/load/gateway_load.py" --until-absent canary --timeout 30 > /dev/null
    kubectl scale deployment "$CANARY" --replicas=0 > /dev/null
    echo "canary: $STABLE now runs $image ($version); canary weight 0, scaled to 0"
}

cmd_status() {
    echo "weight: $(current_weight) %"
    for deployment in "$STABLE" "$CANARY"; do
        kubectl get deployment "$deployment" -o jsonpath="{.metadata.name}: {.spec.template.spec.containers[0].image} version={.spec.template.metadata.labels.version} ready={.status.readyReplicas}/{.spec.replicas}{'\n'}"
    done
}

case "${1:-}" in
    deploy) shift; cmd_deploy "$@" ;;
    rollout) shift; cmd_rollout "$@" ;;
    rollback) cmd_rollback ;;
    promote) cmd_promote ;;
    status) cmd_status ;;
    *) die "usage: $0 {deploy IMAGE VERSION|rollout WEIGHT|rollback|promote|status}" ;;
esac
