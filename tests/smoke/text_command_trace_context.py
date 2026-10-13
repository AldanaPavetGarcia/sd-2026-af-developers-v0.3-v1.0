#!/usr/bin/env python3
"""Smoke test: a text command that carries traceparent= continues the caller's trace (ADR-026).

US-306 AC6 needs a failed operation to produce a span with result=ERROR and an error log with
the same trace_id, both continuing the trace of the request that caused them. This starts a real
eop_server with tracing enabled and points its OTLP exporter at a small HTTP collector in this
process, sends `QUERY_NODE node_id=<unknown> traceparent=<T>`, and checks that:

  1. the server writes an ERROR log line with service=eop-server and trace_id=T;
  2. the exported span carries trace-id T, has the caller's span as parent, and result=ERROR
     (checked on the raw OTLP protobuf bytes: ids are stored as raw bytes and attribute values
     as plain strings, so no protobuf library is needed);
  3. a command without traceparent still works and starts its own trace;
  4. an ANALYZE_GRAPH envelope with traceparent before "payload" continues that trace, and one
     with traceparent after "payload" is traced from the connection instead (the server does not
     scan a multi-megabyte graph to find it).
"""

import json
import os
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

TRACE_ID = "4bf92f3577b34da6a3ce929d0e0e4736"
PARENT_SPAN_ID = "00f067aa0ba902b7"
TRACEPARENT = f"00-{TRACE_ID}-{PARENT_SPAN_ID}-01"
# ANALYZE_GRAPH: honored when it precedes "payload" (as the API gateway sends it), ignored after.
ANALYZE_TRACE_ID = "0af7651916cd43dd8448eb211c80319c"
ANALYZE_TRACEPARENT = f"00-{ANALYZE_TRACE_ID}-b7ad6b7169203331-01"
LATE_TRACE_ID = "1cf7651916cd43dd8448eb211c80319d"
LATE_TRACEPARENT = f"00-{LATE_TRACE_ID}-c7ad6b7169203332-01"
EXPORT_WAIT_SECONDS = 15


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


class Collector(BaseHTTPRequestHandler):
    payloads = []
    lock = threading.Lock()

    def do_POST(self):  # noqa: N802 (http.server naming)
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        with Collector.lock:
            Collector.payloads.append(body)
        self.send_response(200)
        self.send_header("Content-Type", "application/x-protobuf")
        self.end_headers()

    def log_message(self, *_):
        pass


def wait_for_port(port, timeout=8):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.3):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def send(port, line):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        reader = sock.makefile("rb")
        reader.readline()  # WELCOME
        sock.sendall(line.encode() + b"\n")
        return reader.readline().decode().strip()


def check(label, condition, detail=""):
    print(("OK:   " if condition else "FAIL: ") + label + ("" if condition else f" {detail}"))
    return condition


def main():
    if len(sys.argv) != 2:
        print("Usage: text_command_trace_context.py <eop_server_binary>")
        return 2

    collector = HTTPServer(("127.0.0.1", 0), Collector)
    threading.Thread(target=collector.serve_forever, daemon=True).start()

    port = free_port()
    env = os.environ.copy()
    env.update({
        "EOP_PORT": str(port),
        "EOP_METRICS_PORT": str(free_port()),
        "EOP_TRACING_ENABLED": "1",
        "OTEL_EXPORTER_OTLP_ENDPOINT": f"http://127.0.0.1:{collector.server_address[1]}/v1/traces",
    })
    env.pop("OTEL_SERVICE_NAME", None)
    server = subprocess.Popen([sys.argv[1]], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    passed = True
    try:
        if not wait_for_port(port):
            print("FAIL: eop_server did not start")
            return 1

        answer = send(port, f"QUERY_NODE node_id=ghost-node traceparent={TRACEPARENT}")
        passed &= check("unknown node answered with ERROR_NOT_FOUND", answer.startswith("ERROR_NOT_FOUND"), f"(got {answer!r})")
        answer = send(port, "QUERY_NODE node_id=other-ghost")
        passed &= check("command without traceparent still answered", answer.startswith("ERROR_NOT_FOUND"), f"(got {answer!r})")
        graph = {"nodes": ["A", "B"], "edges": [{"from": "A", "to": "B", "weight": 1}], "algorithms": ["connected_components"]}
        # Fields of every JSON type before traceparent: the streaming reader must skip them all.
        early = json.dumps({"type": "ANALYZE_GRAPH", "request_id": "tc-1", "priority": 3, "offset": -1, "ratio": 0.5,
                            "dry_run": False, "note": None, "tags": ["a", 1], "meta": {"k": [True]},
                            "traceparent": ANALYZE_TRACEPARENT, "payload": graph})
        late = json.dumps({"type": "ANALYZE_GRAPH", "request_id": "tc-2", "payload": graph, "traceparent": LATE_TRACEPARENT})
        for envelope in (early, late):
            answer = send(port, envelope)
            passed &= check("ANALYZE_GRAPH without an engine answered", answer.startswith("ERROR_SERVICE_UNAVAILABLE"), f"(got {answer!r})")
        # A malformed envelope must not break trace extraction: it is still answered.
        answer = send(port, '{"type": "ANALYZE_GRAPH", "traceparent": ')
        passed &= check("malformed ANALYZE_GRAPH envelope still answered", answer != "", f"(got {answer!r})")

        deadline = time.time() + EXPORT_WAIT_SECONDS
        trace_bytes = bytes.fromhex(TRACE_ID)
        while time.time() < deadline:
            with Collector.lock:
                if any(trace_bytes in p for p in Collector.payloads):
                    break
            time.sleep(0.2)
        with Collector.lock:
            exported = b"".join(Collector.payloads)
        passed &= check("span exported with the caller's trace-id", trace_bytes in exported)
        passed &= check("span has the caller's span as parent", bytes.fromhex(PARENT_SPAN_ID) in exported)
        passed &= check("span carries result=ERROR", b"result" in exported and b"ERROR" in exported)
        deadline = time.time() + EXPORT_WAIT_SECONDS
        while time.time() < deadline and bytes.fromhex(ANALYZE_TRACE_ID) not in exported:
            time.sleep(0.2)
            with Collector.lock:
                exported = b"".join(Collector.payloads)
        passed &= check("ANALYZE_GRAPH with traceparent before payload continues that trace", bytes.fromhex(ANALYZE_TRACE_ID) in exported)
        passed &= check("ANALYZE_GRAPH with traceparent after payload does not", bytes.fromhex(LATE_TRACE_ID) not in exported)
    finally:
        server.terminate()
        try:
            stdout, _ = server.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
            stdout, _ = server.communicate()
        collector.shutdown()

    error_logs = []
    for line in stdout.splitlines():
        try:
            entry = json.loads(line)
        except ValueError:
            continue
        if entry.get("level") == "ERROR" and entry.get("trace_id") == TRACE_ID:
            error_logs.append(entry)
    passed &= check("ERROR log line carries the caller's trace_id", bool(error_logs), "(no ERROR line with that trace_id)")
    if error_logs:
        passed &= check("log line carries service=eop-server", error_logs[0].get("service") == "eop-server", f"(got {error_logs[0]})")

    if passed:
        print("OK: text command trace context continues the caller's trace")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
