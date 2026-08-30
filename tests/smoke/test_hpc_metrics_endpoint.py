#!/usr/bin/env python3
"""
Smoke test for the hpc-engine Prometheus /metrics endpoint.

The test starts a tiny fake EOP server, launches hpc_engine in socket mode,
waits until the metrics endpoint is reachable, and validates the Prometheus
families required by the HPC Grafana dashboard.
"""

import os
import socket
import subprocess
import sys
import threading
import time
import urllib.request


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def read_line(conn, timeout=5):
    conn.settimeout(timeout)
    data = b""

    while not data.endswith(b"\n"):
        chunk = conn.recv(1)
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


class FakeEopServer:
    def __init__(self, port):
        self.port = port
        self.ready = threading.Event()
        self.registered = threading.Event()
        self.analysis_done = threading.Event()
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)

    def start(self):
        self.thread.start()
        if not self.ready.wait(timeout=5):
            raise RuntimeError("fake EOP server did not start")

    def close(self):
        self.stop.set()
        self.thread.join(timeout=3)

    def run(self):
        with socket.socket() as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind(("127.0.0.1", self.port))
            server.listen(1)
            self.ready.set()

            conn, _ = server.accept()
            with conn:
                register = read_line(conn)
                if not register.startswith("REGISTER "):
                    return

                conn.sendall(b"ACK node_id=hpc-metrics-smoke\n")
                self.registered.set()

                request = (
                    '{"type":"ANALYZE_GRAPH","request_id":"hpc-metrics-smoke-1",'
                    '"payload":{"nodes":["A"],"edges":[],"algorithms":["connected_components"]}}\n'
                )
                conn.sendall(request.encode())

                response = read_line(conn)
                if '"status":"ACK"' in response:
                    self.analysis_done.set()

                while not self.stop.is_set():
                    time.sleep(0.1)


def fetch_metrics(metrics_port):
    with urllib.request.urlopen(f"http://127.0.0.1:{metrics_port}/metrics", timeout=5) as response:
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
        print("Usage: test_hpc_metrics_endpoint.py <hpc_engine_binary>")
        return 2

    hpc_engine = sys.argv[1]
    if not os.path.exists(hpc_engine):
        print(f"ERROR: hpc_engine binary not found: {hpc_engine}")
        return 2

    server_port = free_port()
    metrics_port = free_port()

    fake_server = FakeEopServer(server_port)
    fake_server.start()

    env = os.environ.copy()
    env["EOP_HPC_METRICS_PORT"] = str(metrics_port)

    process = subprocess.Popen(
        [
            hpc_engine,
            "--server-host",
            "127.0.0.1",
            "--server-port",
            str(server_port),
            "--node-id",
            "hpc-metrics-smoke",
            "--retry-max-attempts",
            "1",
            "--retry-base-delay-ms",
            "50",
        ],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        if not fake_server.registered.wait(timeout=5):
            print("ERROR: hpc_engine did not register with the fake EOP server")
            return 1

        if not wait_for_port(metrics_port):
            print("ERROR: hpc_engine Prometheus /metrics endpoint did not start")
            return 1

        if not fake_server.analysis_done.wait(timeout=5):
            print("ERROR: hpc_engine did not process the fake ANALYZE_GRAPH request")
            return 1

        status, body = fetch_metrics(metrics_port)
        if status != 200:
            print(f"ERROR: /metrics returned HTTP {status}, expected 200")
            return 1

        required_substrings = [
            "# TYPE eop_hpc_requests_total counter",
            "# TYPE eop_hpc_processing_duration_seconds histogram",
            "# TYPE eop_hpc_algorithm_duration_seconds histogram",
            "# TYPE eop_hpc_thread_count gauge",
            "# TYPE eop_hpc_graph_nodes gauge",
            "# TYPE eop_hpc_graph_edges gauge",
            'eop_hpc_requests_total{service="hpc-engine",status="ACK"}',
            'eop_hpc_algorithm_duration_seconds_bucket{algorithm="connected_components",service="hpc-engine"',
        ]

        missing = [item for item in required_substrings if item not in body]
        if missing:
            print("ERROR: /metrics response is missing expected Prometheus families:")
            for item in missing:
                print(f"  - {item}")
            print("--- full /metrics response ---")
            print(body)
            return 1

        print("[hpc-metrics-test] passed: hpc_engine exposes Prometheus /metrics")
        return 0

    finally:
        fake_server.close()
        terminate_process(process)


if __name__ == "__main__":
    sys.exit(main())
