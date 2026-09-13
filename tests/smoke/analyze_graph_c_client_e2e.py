#!/usr/bin/env python3
"""
US-201 AC8 / US-203 AC2: "C client -> v0.1 server -> HPC engine -> result delivered to client."

Same process orchestration as analyze_graph_real_engine.py (spawn a real eop_server, spawn a
real hpc_engine in socket mode, wait for registration), but the actual ANALYZE_GRAPH request is
driven by a compiled C client program linked against libeop (analyze_graph_c_client.c), not a
raw Python socket -- the ACs name a C client specifically, and nothing exercised that leg before.
"""

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


def terminate_process(process):
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def main():
    if len(sys.argv) != 4:
        print("Usage: analyze_graph_c_client_e2e.py <eop_server_binary> <hpc_engine_binary> "
              "<c_client_binary>")
        return 2

    server_bin, hpc_bin, c_client_bin = sys.argv[1], sys.argv[2], sys.argv[3]

    for label, path in (("eop_server", server_bin), ("hpc_engine", hpc_bin),
                        ("analyze_graph_c_client", c_client_bin)):
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

        hpc = subprocess.Popen(
            [hpc_bin, "--server-host", "127.0.0.1", "--server-port", str(port),
             "--node-id", "hpc-engine"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )

        if not wait_for_hpc_registration(port):
            print("ERROR: HPC engine did not register with EOP server")
            return 1

        result = subprocess.run(
            [c_client_bin, "127.0.0.1", str(port)],
            capture_output=True, text=True, timeout=10,
        )
        print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, file=sys.stderr, end="")

        if result.returncode != 0:
            print(f"ERROR: C client exited with code {result.returncode}")
            return 1

        return 0

    finally:
        if hpc is not None:
            terminate_process(hpc)
        terminate_process(server)


if __name__ == "__main__":
    sys.exit(main())
