#!/usr/bin/env python3
"""
Smoke test for US-206: Prometheus metrics instrumentation in eop-server.

Spawns a real eop_server binary (no Docker Compose required — wired into CTest the same way
as analyze_graph_real_engine.py), drives a small amount of real TCP protocol traffic, then
scrapes GET /metrics on EOP_METRICS_PORT and validates the Prometheus text-exposition format:
metric names, label sets, and TYPE declarations match what
monitoring/grafana/dashboards/eop-slo-dashboard.json (US-205) queries.
"""

import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request


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


def recv_line(sock, timeout=5):
    sock.settimeout(timeout)
    data = b""

    while not data.endswith(b"\n"):
        chunk = sock.recv(1)
        if not chunk:
            break
        data += chunk

    return data.decode(errors="replace").strip()


def send_and_receive(port, message, timeout=5):
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        # Discard an optional WELCOME banner, matching the other smoke test's convention.
        try:
            first = recv_line(sock, timeout=0.3)
        except Exception:
            first = ""

        sock.sendall((message + "\n").encode())
        response = recv_line(sock, timeout=timeout)
        return first, response


def fetch_metrics(metrics_port, timeout=5):
    url = f"http://127.0.0.1:{metrics_port}/metrics"
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return response.status, response.read().decode(errors="replace")


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
    if len(sys.argv) != 2:
        print("Usage: test_metrics_endpoint.py <eop_server_binary>")
        return 2

    server_bin = sys.argv[1]

    if not os.path.exists(server_bin):
        print(f"ERROR: eop_server binary not found: {server_bin}")
        return 2

    port = free_port()
    metrics_port = free_port()

    env = os.environ.copy()
    env["EOP_PORT"] = str(port)
    env["EOP_METRICS_PORT"] = str(metrics_port)
    env["EOP_TRACING_ENABLED"] = "0"

    server = subprocess.Popen(
        [server_bin],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        if not wait_for_port(port):
            print("ERROR: EOP server did not start (TCP protocol port)")
            return 1

        if not wait_for_port(metrics_port):
            print("ERROR: EOP server did not start the Prometheus /metrics exposer")
            return 1

        print("1. Generating traffic: one successful REGISTER, one failing REGISTER, one HEARTBEAT...")
        _, register_ok = send_and_receive(
            port,
            "REGISTER node_id=metrics-smoke-1 bunker_name=Smoke ip_address=10.0.0.9 capacity=5",
        )
        if "ACK" not in register_ok:
            print(f"ERROR: expected successful REGISTER to ACK, got: {register_ok}")
            return 1

        _, register_err = send_and_receive(port, "REGISTER node_id=metrics-smoke-missing")
        if "ERROR" not in register_err:
            print(f"ERROR: expected malformed REGISTER to fail, got: {register_err}")
            return 1

        _, heartbeat_resp = send_and_receive(port, "HEARTBEAT")
        if "ACK" not in heartbeat_resp:
            print(f"ERROR: expected HEARTBEAT to ACK, got: {heartbeat_resp}")
            return 1

        print("2. Scraping GET /metrics...")
        try:
            status, body = fetch_metrics(metrics_port)
        except urllib.error.URLError as exc:
            print(f"ERROR: could not reach /metrics endpoint: {exc}")
            return 1

        if status != 200:
            print(f"ERROR: /metrics returned status {status}, expected 200")
            return 1

        print("3. Validating Prometheus exposition format...")

        required_substrings = [
            "# TYPE eop_requests_total counter",
            "# TYPE eop_request_duration_seconds histogram",
            'eop_requests_total{command="REGISTER",result="OK",service="eop-server",status="OK"}',
            'eop_requests_total{command="REGISTER",result="ERROR",service="eop-server",status="ERROR"}',
            'eop_requests_total{command="HEARTBEAT",result="OK",service="eop-server",status="OK"}',
            'eop_request_duration_seconds_bucket{command="REGISTER",service="eop-server"',
            'eop_request_duration_seconds_sum{command="REGISTER",service="eop-server"}',
            'eop_request_duration_seconds_count{command="REGISTER",service="eop-server"}',
        ]

        missing = [needle for needle in required_substrings if needle not in body]
        if missing:
            print("ERROR: /metrics response is missing expected content:")
            for needle in missing:
                print(f"  - {needle}")
            print("--- full /metrics response ---")
            print(body)
            return 1

        print("OK: /metrics exposes eop_requests_total and eop_request_duration_seconds "
              "with the labels the US-205 SLO dashboard queries")
        return 0

    finally:
        terminate_process(server)


if __name__ == "__main__":
    sys.exit(main())
