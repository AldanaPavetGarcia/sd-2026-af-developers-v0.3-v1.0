#!/usr/bin/env bash
set -euo pipefail

EOP_HOST="${EOP_HOST:-127.0.0.1}"
EOP_PORT="${EOP_PORT:-9026}"
SMOKE_TIMEOUT_SECONDS="${SMOKE_TIMEOUT_SECONDS:-60}"

echo "Waiting for EOP server at ${EOP_HOST}:${EOP_PORT}..."

deadline=$((SECONDS + SMOKE_TIMEOUT_SECONDS))

while (( SECONDS < deadline )); do
    if timeout 2 bash -c "cat < /dev/null > /dev/tcp/${EOP_HOST}/${EOP_PORT}" 2>/dev/null; then
        echo "EOP server is accepting TCP connections at ${EOP_HOST}:${EOP_PORT}."
        exit 0
    fi

    sleep 1
done    

echo "ERROR: EOP server did not accept TCP connections within ${SMOKE_TIMEOUT_SECONDS}s." >&2
exit 1
EOF

chmod +x scripts/smoke/deploy-local-smoke.sh