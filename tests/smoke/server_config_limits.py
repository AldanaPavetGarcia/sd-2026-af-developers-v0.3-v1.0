#!/usr/bin/env python3
"""Smoke test: eop_server's limits and its fallback on unusable configuration.

Each case starts its own eop_server, because the configuration is read once at startup.

  1. EOP_MAX_REQUEST_BYTES caps the buffer of a message still waiting for its '\\n': a client
     that sends more than that without a terminator gets ERROR_BAD_REQUEST, and the server keeps
     serving other clients.
  2. Malformed or non-positive EOP_MAX_CLIENTS, EOP_HEARTBEAT_INTERVAL, EOP_IDLE_TIMEOUT and
     EOP_MAX_REQUEST_BYTES do not stop the server: it starts with its defaults (30 s idle
     timeout, 100 clients, 5 s heartbeat) and serves requests.
"""

import os
import socket
import subprocess
import sys
import time

MAX_REQUEST_BYTES = 1024
DEFAULTS_BANNER = "Inactivity timeout: 30s | Max clients: 100 | Heartbeat interval: 5s"
UNUSABLE_CONFIGS = [
    {"EOP_MAX_CLIENTS": "many", "EOP_HEARTBEAT_INTERVAL": "-3", "EOP_IDLE_TIMEOUT": "soon",
     "EOP_MAX_REQUEST_BYTES": "12abc"},
    {"EOP_MAX_CLIENTS": "0", "EOP_HEARTBEAT_INTERVAL": "often", "EOP_IDLE_TIMEOUT": "-1",
     "EOP_MAX_REQUEST_BYTES": "0"},
]


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def wait_for_port(port, timeout=8):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.3):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def connect(port):
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    reader = sock.makefile("rb")
    reader.readline()  # WELCOME
    return sock, reader


def ask(port, line):
    sock, reader = connect(port)
    with sock:
        sock.sendall(line.encode() + b"\n")
        return reader.readline().decode().strip()


def check(label, condition, detail=""):
    print(("OK:   " if condition else "FAIL: ") + label + ("" if condition else f" {detail}"))
    return condition


def start_server(server_bin, extra_env):
    port = free_port()
    env = os.environ.copy()
    for key in ("EOP_MAX_CLIENTS", "EOP_HEARTBEAT_INTERVAL", "EOP_IDLE_TIMEOUT", "EOP_MAX_REQUEST_BYTES"):
        env.pop(key, None)
    env.update({"EOP_PORT": str(port), "EOP_METRICS_PORT": str(free_port()), "EOP_TRACING_ENABLED": "0"})
    env.update(extra_env)
    server = subprocess.Popen([server_bin], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return server, port


def stop_server(server):
    server.terminate()
    try:
        stdout, _ = server.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        server.kill()
        stdout, _ = server.communicate()
    return stdout


def oversized_unterminated_message(server_bin):
    server, port = start_server(server_bin, {"EOP_MAX_REQUEST_BYTES": str(MAX_REQUEST_BYTES)})
    passed = True
    try:
        if not wait_for_port(port):
            return check("server started with EOP_MAX_REQUEST_BYTES", False)
        sock, reader = connect(port)
        with sock:
            # Sent in two parts, so the cap is hit while reassembling, not in a single read.
            sock.sendall(b"QUERY_NODE node_id=" + b"x" * (MAX_REQUEST_BYTES // 2))
            time.sleep(0.2)
            sock.sendall(b"x" * MAX_REQUEST_BYTES)
            answer = reader.readline().decode().strip()
        passed &= check("unterminated message over the cap is rejected",
                        answer == "ERROR_BAD_REQUEST desc=Message too long", f"(got {answer!r})")
        answer = ask(port, "QUERY_NODE node_id=after-the-cap")
        passed &= check("server keeps serving after rejecting it", answer.startswith("ERROR_NOT_FOUND"), f"(got {answer!r})")
    finally:
        stop_server(server)
    return passed


def unusable_config(server_bin, extra_env):
    server, port = start_server(server_bin, extra_env)
    passed = True
    try:
        started = wait_for_port(port)
        passed &= check(f"server starts with {extra_env}", started)
        if started:
            answer = ask(port, "REGISTER node_id=cfg-node bunker_name=Cfg ip_address=10.0.0.1 capacity=1")
            passed &= check("and serves requests", answer.startswith("ACK"), f"(got {answer!r})")
    finally:
        stdout = stop_server(server)
    passed &= check("it falls back to the default limits", DEFAULTS_BANNER in stdout,
                    f"(banner not found in output: {stdout[:300]!r})")
    return passed


def main():
    if len(sys.argv) != 2:
        print("Usage: server_config_limits.py <eop_server_binary>")
        return 2

    passed = oversized_unterminated_message(sys.argv[1])
    for extra_env in UNUSABLE_CONFIGS:
        passed &= unusable_config(sys.argv[1], extra_env)

    if passed:
        print("OK: eop_server enforces its limits and survives unusable configuration")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
