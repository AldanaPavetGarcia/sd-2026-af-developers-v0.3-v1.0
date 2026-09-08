#!/usr/bin/env python3
"""
US-203 AC6: "An integration test verifies: engine offline -> client receives error -> engine
comes online -> client retries -> result delivered."

The unit test suite already covers each state in isolation (AnalyzeGraphReturnsServiceUnavailable
WhenNoHpcEngineIsRegistered, AnalyzeGraphForwardsRequestToRegisteredHpcEngine,
AnalyzeGraphReturnsTimeoutWhenHpcEngineStalls, tests/unit/testConnection.cpp) but nothing
exercises the full transition as one continuous sequence against the real binaries.
"""

import json
import os
import socket
import subprocess
import sys
import time


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def recv_line(sock, timeout=5):
    sock.settimeout(timeout)
    data = b""
    while not data.endswith(b"\n"):
        chunk = sock.recv(1)
        if not chunk:
            break
        data += chunk
    return data.decode(errors="replace").strip()


def wait_for_port(port, timeout=8):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.3):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def discard_optional_welcome(sock):
    try:
        first_line = recv_line(sock, timeout=0.3)
        if first_line and not first_line.startswith("WELCOME"):
            return first_line
    except Exception:
        pass
    return None


def wait_for_hpc_registration(port, timeout=8):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1) as sock:
                pending_line = discard_optional_welcome(sock)
                sock.sendall(b"LIST_NODES\n")

                lines = []
                if pending_line:
                    lines.append(pending_line)

                while True:
                    line = recv_line(sock, timeout=1)
                    if not line:
                        break
                    lines.append(line)
                    if line == "END_LIST":
                        break

                for line in lines:
                    if "node_id=hpc-engine" in line and "status=ONLINE" in line:
                        return True
        except OSError:
            pass
        time.sleep(0.2)
    return False


def send_analyze_graph(port, request_id):
    request = {
        "type": "ANALYZE_GRAPH",
        "request_id": request_id,
        "payload": {
            "nodes": ["A", "B"],
            "edges": [{"from": "A", "to": "B", "weight": 1}],
            "algorithms": ["connected_components"],
        },
    }
    with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
        discard_optional_welcome(client)
        client.sendall((json.dumps(request, separators=(",", ":")) + "\n").encode())
        return recv_line(client, timeout=5)


def terminate_process(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def main():
    if len(sys.argv) != 3:
        print("Usage: analyze_graph_offline_online_cycle.py <eop_server_binary> "
              "<hpc_engine_binary>")
        return 2

    server_bin, hpc_bin = sys.argv[1], sys.argv[2]
    for label, path in (("eop_server", server_bin), ("hpc_engine", hpc_bin)):
        if not os.path.exists(path):
            print(f"ERROR: {label} binary not found: {path}")
            return 2

    port = free_port()
    env = os.environ.copy()
    env["EOP_PORT"] = str(port)
    env["EOP_TRACING_ENABLED"] = "0"
    env["EOP_HPC_TIMEOUT_MS"] = "3000"

    server = subprocess.Popen(
        [server_bin], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    hpc = None

    try:
        if not wait_for_port(port):
            print("ERROR: EOP server did not start")
            return 1

        # 1. Engine offline -> client receives a structured error, not a hang/crash.
        response = send_analyze_graph(port, "offline-online-1")
        print("Step 1 (engine offline):", response)
        if "ERROR_SERVICE_UNAVAILABLE" not in response:
            print("ERROR: expected ERROR_SERVICE_UNAVAILABLE while the engine is offline")
            return 1

        # 2. Engine comes online.
        hpc = subprocess.Popen(
            [hpc_bin, "--server-host", "127.0.0.1", "--server-port", str(port),
             "--node-id", "hpc-engine"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        if not wait_for_hpc_registration(port):
            print("ERROR: HPC engine did not register with EOP server")
            return 1

        # 3. Client retries the same kind of request -> result delivered this time.
        response = send_analyze_graph(port, "offline-online-2")
        print("Step 2 (engine online, retry):", response)
        if '"type":"RESPONSE"' not in response or '"status":"ACK"' not in response:
            print("ERROR: expected a successful RESPONSE once the engine is online")
            return 1
        if '"request_id":"offline-online-2"' not in response:
            print("ERROR: response request_id mismatch")
            return 1

        print("OK: offline -> error -> online -> retry -> result delivered")
        return 0

    finally:
        terminate_process(hpc)
        terminate_process(server)


if __name__ == "__main__":
    sys.exit(main())
