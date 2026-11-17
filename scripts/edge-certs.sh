#!/usr/bin/env bash
# Generates the TLS material of the broker's device listener (8883, ADR-013) and, with --k8s,
# the mosquitto-tls Secret the broker mounts. Keys stay on this machine: certs/ is ignored by git.
#
#   scripts/edge-certs.sh [--k8s]        EOP_EDGE_CERTS_DIR (certs/edge)  EOP_EDGE_CERT_DAYS (825)
#
# Devices verify the broker with ca.crt; the server certificate names the in-cluster Service and,
# when minikube runs, its IP (devices connect to <minikube ip>:30883).
set -euo pipefail

DIR="${EOP_EDGE_CERTS_DIR:-certs/edge}"
DAYS="${EOP_EDGE_CERT_DAYS:-825}"
NAMESPACE="${EOP_K8S_NAMESPACE:-default}"
mkdir -p "$DIR"
chmod 700 "$DIR"

if [[ ! -f "$DIR/ca.key" ]]; then
    openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days "$DAYS" \
        -keyout "$DIR/ca.key" -out "$DIR/ca.crt" -subj "/CN=EOP edge CA" 2>/dev/null
fi
if [[ ! -f "$DIR/tls.key" ]]; then
    SANS="DNS:mosquitto,DNS:mosquitto.${NAMESPACE}.svc,DNS:mosquitto.${NAMESPACE}.svc.cluster.local,DNS:localhost,IP:127.0.0.1"
    if command -v minikube >/dev/null && ip="$(minikube ip 2>/dev/null)"; then
        SANS="${SANS},IP:${ip}"
    fi
    openssl req -newkey rsa:2048 -nodes -sha256 -keyout "$DIR/tls.key" -out "$DIR/tls.csr" \
        -subj "/CN=mosquitto" 2>/dev/null
    openssl x509 -req -in "$DIR/tls.csr" -CA "$DIR/ca.crt" -CAkey "$DIR/ca.key" -CAcreateserial \
        -days "$DAYS" -sha256 -out "$DIR/tls.crt" -extfile <(printf 'subjectAltName=%s\n' "$SANS") 2>/dev/null
    rm -f "$DIR/tls.csr"
fi
chmod 600 "$DIR"/*.key
echo "edge-certs: $DIR/ca.crt (give it to the devices), $DIR/tls.crt, $DIR/tls.key"

if [[ "${1:-}" == "--k8s" ]]; then
    kubectl -n "$NAMESPACE" create secret generic mosquitto-tls \
        --from-file=ca.crt="$DIR/ca.crt" --from-file=tls.crt="$DIR/tls.crt" --from-file=tls.key="$DIR/tls.key" \
        --dry-run=client -o yaml | kubectl -n "$NAMESPACE" apply -f - >/dev/null
    echo "edge-certs: Secret mosquitto-tls applied in namespace $NAMESPACE"
fi
