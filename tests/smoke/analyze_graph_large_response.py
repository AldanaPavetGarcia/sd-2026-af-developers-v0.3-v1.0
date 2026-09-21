#!/usr/bin/env python3
"""
Regression test: an ANALYZE_GRAPH response larger than one socket read reaches the client whole.

The server used to read the engine's reply with a single 64 KB recv(), so any larger response was
cut at 65,535 bytes and the remainder stayed in the engine socket, to be delivered as the answer to
the *next* request. Every other smoke test uses graphs of a few nodes, whose responses fit in one
read, so none of them could see it; it surfaced end to end with a 10K-node graph (US-201 AC3).

Sends a graph whose response is several times 64 KB, then a small second request on the same
connection, and checks that both responses are complete JSON carrying their own request_id.
"""
import json
import os
import socket
import subprocess
import sys
import time

LARGE_GRAPH_NODES = 3000        # centrality for 3000 nodes serializes to well over 64 KB
READ_CHUNK_BYTES = 1 << 16
RESPONSE_TIMEOUT_S = 60
ONE_READ_BYTES = 65536


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_for_port(port, timeout=8):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.3):
                return True
        except OSError:
            time.sleep(0.1)
    return False


class LineReader:
    """Reads newline-terminated messages, keeping any bytes past the terminator for the next one."""

    def __init__(self, sock):
        self.sock = sock
        self.buffer = b""

    def line(self, timeout):
        self.sock.settimeout(timeout)
        while b"\n" not in self.buffer:
            chunk = self.sock.recv(READ_CHUNK_BYTES)
            if not chunk:
                raise ConnectionError(f"connection closed after {len(self.buffer)} bytes without a terminator")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        return line


def wait_for_engine(port, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1) as sock:
                reader = LineReader(sock)
                reader.line(timeout=1)  # WELCOME
                sock.sendall(b"LIST_NODES\n")
                while True:
                    line = reader.line(timeout=1)
                    if b"node_id=hpc-engine" in line and b"status=ONLINE" in line:
                        return True
                    if line == b"END_LIST":
                        break
        except OSError:
            pass
        time.sleep(0.2)
    return False


def graph_request(request_id, nodes):
    names = [f"N{i}" for i in range(nodes)]
    edges = [{"from": names[i], "to": names[i + 1], "weight": 1} for i in range(nodes - 1)]
    return {"type": "ANALYZE_GRAPH", "request_id": request_id,
            "payload": {"nodes": names, "edges": edges, "algorithms": ["connected_components", "centrality"]}}


def terminate(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


def main():
    if len(sys.argv) != 3:
        print("Usage: analyze_graph_large_response.py <eop_server_binary> <hpc_engine_binary>")
        return 2

    port = free_port()
    env = dict(os.environ, EOP_PORT=str(port), EOP_TRACING_ENABLED="0",
               EOP_HPC_TIMEOUT_MS=str(RESPONSE_TIMEOUT_S * 1000))
    server = subprocess.Popen([sys.argv[1]], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    engine = None
    try:
        if not wait_for_port(port):
            print("ERROR: EOP server did not start")
            return 1
        engine = subprocess.Popen([sys.argv[2], "--server-host", "127.0.0.1", "--server-port", str(port),
                                   "--node-id", "hpc-engine"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not wait_for_engine(port):
            print("ERROR: HPC engine did not register with EOP server")
            return 1

        with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
            reader = LineReader(sock)
            reader.line(timeout=5)  # WELCOME

            for request_id, nodes in (("large-response", LARGE_GRAPH_NODES), ("after-large", 3)):
                request = graph_request(request_id, nodes)
                sock.sendall((json.dumps(request, separators=(",", ":")) + "\n").encode())
                raw = reader.line(timeout=RESPONSE_TIMEOUT_S)
                try:
                    response = json.loads(raw)
                except ValueError:
                    print(f"ERROR: {request_id}: response is not complete JSON ({len(raw)} bytes)")
                    return 1
                if response.get("request_id") != request_id or response.get("status") != "ACK":
                    print(f"ERROR: {request_id}: got request_id={response.get('request_id')} "
                          f"status={response.get('status')} -- a previous response leaked into this one")
                    return 1
                if request_id == "large-response" and len(raw) <= ONE_READ_BYTES:
                    print(f"ERROR: response is only {len(raw)} bytes; the test no longer exceeds one read")
                    return 1
                print(f"OK: {request_id}: {len(raw)} bytes, complete and correlated")

        print("OK: responses larger than one socket read are relayed whole and in order")
        return 0
    finally:
        terminate(engine)
        terminate(server)


if __name__ == "__main__":
    sys.exit(main())
