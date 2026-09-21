#!/usr/bin/env python3

"""Verifies W3C trace context reaches the HPC Engine (US-203 AC5 / US-206 AC5).

docs/analyze-graph-protocol-v0.2.md defines a `traceparent` field on the ANALYZE_GRAPH envelope
so engine-side work joins the trace of the request that caused it. The server never populated it:
the engine parsed the field and echoed it back, but nothing ever set it, so the engine had no
parent to attach its span to.

Because the engine echoes the traceparent it received, the response is proof of what the server
actually sent -- no collector needed. This asserts the echoed value is a well-formed W3C
traceparent whose trace-id matches the one the server logged for the same request, which is only
possible if the server injected its own live trace context.
"""

import json
import os
import re
import socket
import subprocess
import sys
import time

# 00-<32 hex trace-id>-<16 hex span-id>-<2 hex flags>
TRACEPARENT_RE = re.compile(r"^00-([0-9a-f]{32})-([0-9a-f]{16})-[0-9a-f]{2}$")
ALL_ZERO_TRACE_ID = "0" * 32
ALL_ZERO_SPAN_ID = "0" * 16

# Nothing listens here: the point is to exercise real span creation without needing a collector.
# Export failures are retried in the background and never affect the response path.
UNUSED_OTLP_ENDPOINT = "http://127.0.0.1:59999/v1/traces"


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def wait_for_port(port, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.3):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def recv_line(sock, timeout=25):
    """Reads exactly one '\\n'-terminated line.

    Deliberately one byte at a time: a bulk read would pull several lines into one buffer, and
    the LIST_NODES reply below is parsed line by line.
    """
    sock.settimeout(timeout)
    data = b""
    while not data.endswith(b"\n"):
        chunk = sock.recv(1)
        if not chunk:
            break
        data += chunk
    return data.decode(errors="replace").strip()


def discard_welcome(sock):
    sock.settimeout(0.5)
    try:
        while True:
            if not sock.recv(4096):
                break
    except socket.timeout:
        pass
    sock.settimeout(25)


def wait_for_engine_registration(port, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1) as sock:
                discard_welcome(sock)
                sock.sendall(b"LIST_NODES\n")
                lines = []
                while True:
                    line = recv_line(sock, timeout=2)
                    if not line:
                        break
                    lines.append(line)
                    if line == "END_LIST":
                        break
                if any("node_id=hpc-engine" in line and "status=ONLINE" in line for line in lines):
                    return True
        except OSError:
            pass
        time.sleep(0.3)
    return False


def check(label, condition, detail=""):
    if condition:
        print(f"  OK: {label}")
        return True
    print(f"  FAIL: {label} {detail}")
    return False


def main():
    if len(sys.argv) < 3:
        print("usage: trace_context_propagation.py <path-to-eop_server> <path-to-hpc_engine>")
        return 2

    server_bin, engine_bin = sys.argv[1], sys.argv[2]
    port = free_port()

    env = os.environ.copy()
    env["EOP_PORT"] = str(port)
    env["EOP_IDLE_TIMEOUT"] = "60"
    # Tracing must be on: with it disabled there is no active span, so there is deliberately no
    # trace context to propagate and this contract does not apply.
    env["EOP_TRACING_ENABLED"] = "1"
    env["OTEL_EXPORTER_OTLP_ENDPOINT"] = UNUSED_OTLP_ENDPOINT

    server = subprocess.Popen(
        [server_bin], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )

    engine = None
    try:
        if not wait_for_port(port):
            print("ERROR: server did not start listening")
            return 1

        engine = subprocess.Popen(
            [
                engine_bin,
                "--server-host", "127.0.0.1",
                "--server-port", str(port),
                "--node-id", "hpc-engine",
            ],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        if not wait_for_engine_registration(port):
            print("ERROR: hpc-engine did not register with the server")
            return 1

        request = {
            "type": "ANALYZE_GRAPH",
            "request_id": "trace-propagation-check",
            "payload": {
                "nodes": ["A", "B", "C"],
                "edges": [
                    {"from": "A", "to": "B", "weight": 1},
                    {"from": "B", "to": "C", "weight": 2},
                ],
                "algorithms": ["connected_components"],
            },
        }

        with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
            discard_welcome(sock)
            sock.sendall((json.dumps(request, separators=(",", ":")) + "\n").encode())
            raw_response = recv_line(sock)

        print(f"  response: {raw_response[:120]}")

        response = json.loads(raw_response)
        passed = check("request succeeded", response.get("status") == "ACK", f"(got {response.get('status')!r})")

        echoed = response.get("traceparent", "")
        passed &= check(
            "engine received a traceparent (server injected one)",
            bool(echoed),
            "-- response carries no traceparent, so the server sent none",
        )

        if echoed:
            match = TRACEPARENT_RE.match(echoed)
            passed &= check(
                "traceparent is a well-formed W3C value",
                match is not None,
                f"(got {echoed!r})",
            )

            if match:
                trace_id, span_id = match.group(1), match.group(2)
                passed &= check(
                    "trace-id is not the all-zero (invalid) id",
                    trace_id != ALL_ZERO_TRACE_ID,
                    f"(got {trace_id!r})",
                )
                passed &= check(
                    "span-id is not the all-zero (invalid) id",
                    span_id != ALL_ZERO_SPAN_ID,
                    f"(got {span_id!r})",
                )

                # The server logs trace_id per request; the value it propagated must be the trace
                # it was actually in, not a freshly minted one. Its graceful shutdown can take
                # several seconds, so the read is given room and falls back to a kill.
                server.terminate()
                try:
                    stdout, _ = server.communicate(timeout=25)
                except subprocess.TimeoutExpired:
                    server.kill()
                    stdout, _ = server.communicate(timeout=10)
                server = None
                passed &= check(
                    "propagated trace-id matches the server's own trace for this request",
                    trace_id in stdout,
                    f"(trace-id {trace_id} not found in the server's ANALYZE_GRAPH logs)",
                )

        if not passed:
            print("FAILED: trace context is not propagated to the HPC Engine")
            return 1

        print("OK: trace context propagates from eop-server to hpc-engine")
        return 0
    finally:
        for proc in (engine, server):
            if proc is None:
                continue
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    sys.exit(main())
