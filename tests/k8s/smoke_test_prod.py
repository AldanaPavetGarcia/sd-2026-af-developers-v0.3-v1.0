#!/usr/bin/env python3
"""Smoke test of the deployed EOP v1.0 product (US-406 AC8): `make smoke-test-prod`.

Against the stack of deploy/production in the current kubectl context, it checks:

  1. server connectivity: a TCP client of the eop-server Service gets LIST_NODES ... END_LIST;
  2. node registration: REGISTER answers ACK and the node is visible ONLINE through the API
     gateway while its session lives (the registry is shared by both replicas, ADR-027);
  3. graph analysis: POST /v1/graph/analyze answers 200 with the right components (hpc-engine);
  4. edge ingestion: a one-shot edge-sim Pod publishes 3 samples to Mosquitto, the edge
     ingestor forwards them, and /v1/nodes shows the device as an edge node with telemetry;
  5. API gateway health: GET /v1/health answers 200 with status ok.

The Services are reached with `kubectl port-forward`; the edge-sim Pod runs in the cluster.
Exits 0 only if every check passes. Environment: EOP_K8S_NAMESPACE (default), EOP_SMOKE_TIMEOUT
seconds per wait (60), EOP_EDGE_SIM_IMAGE (edge-sim:local).
"""

import json
import os
import socket
import subprocess
import sys
import time
import urllib.parse

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "smoke"))
from gateway_smoke import SAMPLE_GRAPH, call  # noqa: E402

NAMESPACE = os.environ.get("EOP_K8S_NAMESPACE", "default")
TIMEOUT = float(os.environ.get("EOP_SMOKE_TIMEOUT", "60"))
EDGE_SIM_IMAGE = os.environ.get("EOP_EDGE_SIM_IMAGE", "edge-sim:local")
EOP_PORT = 9026
GATEWAY_PORT = 80
EDGE_SAMPLES = 3
POLL_SECONDS = 1.0


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def kubectl(*args, check=True, timeout=None):
    return subprocess.run(["kubectl", "-n", NAMESPACE, *args], capture_output=True, text=True,
                          check=check, timeout=timeout)


class PortForward:
    """kubectl port-forward to a Service, alive for the duration of a with-block."""

    def __init__(self, service, remote):
        self.local = free_port()
        self.proc = subprocess.Popen(
            ["kubectl", "-n", NAMESPACE, "port-forward", f"svc/{service}", f"{self.local}:{remote}"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    def __enter__(self):
        deadline = time.time() + TIMEOUT
        while time.time() < deadline:
            try:
                socket.create_connection(("127.0.0.1", self.local), timeout=1).close()
                return self
            except OSError:
                if self.proc.poll() is not None:
                    raise RuntimeError(f"port-forward exited: {self.proc.stdout.read()}")
                time.sleep(0.2)
        raise RuntimeError("port-forward did not open")

    def __exit__(self, *exc):
        self.proc.terminate()
        self.proc.wait()


def read_until(sock, marker):
    data = b""
    deadline = time.time() + TIMEOUT
    while marker not in data and time.time() < deadline:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
    return data.decode(errors="replace")


class Smoke:
    def __init__(self):
        self.failures = 0

    def check(self, label, ok, detail=""):
        print(f"[{'PASS' if ok else 'FAIL'}] {label}" + (f": {detail}" if detail and not ok else ""))
        if not ok:
            self.failures += 1
        return ok

    def server(self, port, node_id):
        with socket.create_connection(("127.0.0.1", port), timeout=TIMEOUT) as sock:
            sock.sendall(b"LIST_NODES\n")
            answer = read_until(sock, b"END_LIST\n")
        self.check("1. eop-server answers LIST_NODES", "END_LIST" in answer, answer[-200:])

        sock = socket.create_connection(("127.0.0.1", port), timeout=TIMEOUT)
        sock.sendall(f"REGISTER node_id={node_id} bunker_name=Smoke ip_address=10.0.0.1 capacity=1\n".encode())
        answer = read_until(sock, b"\n")
        self.check("2a. REGISTER answers ACK", "ACK" in answer and "ERROR" not in answer, answer)
        return sock  # kept open: the node stays ONLINE while the gateway looks it up

    def registered(self, gateway, node_id):
        status, _, body = call(gateway, "GET", f"/v1/nodes/{urllib.parse.quote(node_id)}")
        node = (body or {}).get("data", {}) if isinstance(body, dict) else {}
        self.check("2b. the registered node is ONLINE through the gateway",
                   status == 200 and node.get("status") == "ONLINE", f"{status} {body}")

    def analysis(self, gateway):
        status, _, body = call(gateway, "POST", "/v1/graph/analyze", body=SAMPLE_GRAPH)
        text = json.dumps(body)
        self.check("3. graph analysis answers 200 with the components",
                   status == 200 and "vault-a" in text and "vault-c" in text, f"{status} {text[:300]}")

    def edge(self, gateway, prefix):
        pod = prefix.rstrip("-")
        kubectl("delete", "pod", pod, "--ignore-not-found", check=False)
        kubectl("run", pod, f"--image={EDGE_SIM_IMAGE}", "--image-pull-policy=IfNotPresent",
                "--restart=Never", "--env=EOP_MQTT_BROKER=tcp://mosquitto:1883",
                "--env=EOP_SIM_DEVICES=1", f"--env=EOP_SIM_COUNT={EDGE_SAMPLES}",
                f"--env=EOP_SIM_NODE_PREFIX={prefix}")
        node_id = f"{prefix}1"
        deadline = time.time() + TIMEOUT
        node = {}
        while time.time() < deadline:
            status, _, body = call(gateway, "GET", f"/v1/nodes/{urllib.parse.quote(node_id)}")
            node = (body or {}).get("data", {}) if isinstance(body, dict) else {}
            if status == 200 and node.get("telemetry"):
                break
            time.sleep(POLL_SECONDS)
        kubectl("delete", "pod", pod, "--ignore-not-found", "--wait=false", check=False)
        self.check("4. an edge sample reaches /v1/nodes through Mosquitto and the ingestor",
                   node.get("source") == "edge" and bool(node.get("telemetry")), json.dumps(node))

    def health(self, gateway):
        status, _, body = call(gateway, "GET", "/v1/health")
        data = (body or {}).get("data", {}) if isinstance(body, dict) else {}
        self.check("5. gateway health is ok", status == 200 and data.get("status") == "ok", f"{status} {body}")


def main():
    stamp = time.strftime("%Y%m%d%H%M%S")
    smoke = Smoke()
    with PortForward("eop-server", EOP_PORT) as server, PortForward("api-gateway", GATEWAY_PORT) as gw:
        gateway = f"http://127.0.0.1:{gw.local}"
        node_id = f"smoke-prod-{stamp}"
        session = smoke.server(server.local, node_id)
        try:
            smoke.registered(gateway, node_id)
        finally:
            session.close()
        smoke.analysis(gateway)
        smoke.edge(gateway, f"smoke-edge-{stamp}-")
        smoke.health(gateway)
    if smoke.failures:
        print(f"smoke-test-prod: {smoke.failures} check(s) failed")
        return 1
    print("smoke-test-prod: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
