#!/usr/bin/env bash
set -euo pipefail

EOP_HOST="${EOP_HOST:-127.0.0.1}"
EOP_PORT="${EOP_PORT:-9026}"
EOP_NODE_ID="${EOP_NODE_ID:-us109-smoke-node-$(date +%s)-$$}"
EOP_BUNKER_NAME="${EOP_BUNKER_NAME:-US109Smoke}"
EOP_NODE_IP="${EOP_NODE_IP:-10.109.0.1}"
EOP_CAPACITY="${EOP_CAPACITY:-109}"
EOP_CONNECT_TIMEOUT_SECONDS="${EOP_CONNECT_TIMEOUT_SECONDS:-5}"
EOP_RESPONSE_TIMEOUT_SECONDS="${EOP_RESPONSE_TIMEOUT_SECONDS:-5}"

if ! command -v python3 >/dev/null 2>&1; then
    echo "ERROR: python3 is required to run the EOP register smoke test." >&2
    exit 1
fi

python3 - \
    "${EOP_HOST}" \
    "${EOP_PORT}" \
    "${EOP_NODE_ID}" \
    "${EOP_BUNKER_NAME}" \
    "${EOP_NODE_IP}" \
    "${EOP_CAPACITY}" \
    "${EOP_CONNECT_TIMEOUT_SECONDS}" \
    "${EOP_RESPONSE_TIMEOUT_SECONDS}" <<'PY'
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
node_id = sys.argv[3]
bunker_name = sys.argv[4]
node_ip = sys.argv[5]
capacity = sys.argv[6]
connect_timeout = float(sys.argv[7])
response_timeout = float(sys.argv[8])

register_command = (
    f"REGISTER node_id={node_id} "
    f"bunker_name={bunker_name} "
    f"ip_address={node_ip} "
    f"capacity={capacity}\n"
)

print(f"Connecting to EOP server at {host}:{port}...")
print(f"Registering node_id={node_id}...")

try:
    with socket.create_connection((host, port), timeout=connect_timeout) as sock:
        sock.settimeout(response_timeout)
        sock.sendall(register_command.encode("utf-8"))

        deadline = time.monotonic() + response_timeout
        buffer = ""
        received_lines = []

        while time.monotonic() < deadline:
            remaining = max(0.1, deadline - time.monotonic())
            sock.settimeout(remaining)

            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                break

            if not chunk:
                print("ERROR: server closed the connection before ACK was received.", file=sys.stderr)
                if received_lines:
                    print("Received before close:", file=sys.stderr)
                    for line in received_lines:
                        print(f"  {line}", file=sys.stderr)
                sys.exit(1)

            buffer += chunk.decode("utf-8", errors="replace")

            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                line = line.strip("\r")

                if not line:
                    continue

                received_lines.append(line)
                print(f"Received: {line}")

                if line.startswith("ACK"):
                    try:
                        sock.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    print("REGISTER smoke test passed.")
                    sys.exit(0)

                if line.startswith("ERROR") or line.startswith("REJECTED"):
                    print(f"ERROR: server rejected REGISTER: {line}", file=sys.stderr)
                    sys.exit(1)

        if buffer.strip():
            received_lines.append(buffer.strip())
            print(f"Received partial: {buffer.strip()}")

        print("ERROR: timeout waiting for REGISTER ACK.", file=sys.stderr)
        if received_lines:
            print("Received before timeout:", file=sys.stderr)
            for line in received_lines:
                print(f"  {line}", file=sys.stderr)
        sys.exit(1)

except OSError as exc:
    print(f"ERROR: could not connect/register against EOP server: {exc}", file=sys.stderr)
    sys.exit(1)
PY
