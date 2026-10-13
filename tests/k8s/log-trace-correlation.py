#!/usr/bin/env python3
"""US-306 AC6 against a running cluster: one failed operation, one trace_id, in SigNoz and Loki.

Requires the EOP stack deployed with `make k8s-apply` (api-gateway, eop-server, Loki, the
otel-log-agent DaemonSet and the SigNoz stack). Steps:

  1. GET /v1/nodes/<unknown node> through the API gateway → 404; the response's meta.trace_id is
     the trace of that request.
  2. Loki: an ERROR line from eop-server with that trace_id (the failed QUERY_NODE).
  3. SigNoz (its ClickHouse store): the eop-server span QUERY_NODE in the same trace, with
     result=ERROR, whose parent is a span of the API gateway — one trace across both services.

Usage: python3 tests/k8s/log-trace-correlation.py
"""

import json
import socket
import subprocess
import sys
import time
import urllib.parse
import urllib.request
import uuid

WAIT_SECONDS = 120
POLL_SECONDS = 3


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def port_forward(target, remote_port):
    port = free_port()
    proc = subprocess.Popen(["kubectl", "port-forward", target, f"{port}:{remote_port}"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return proc, port
        except OSError:
            time.sleep(0.2)
    proc.kill()
    raise RuntimeError(f"port-forward to {target} did not come up")


def get_json(url):
    try:
        with urllib.request.urlopen(url, timeout=10) as resp:
            return resp.status, json.load(resp)
    except urllib.error.HTTPError as err:
        return err.code, json.load(err)


def loki_lines(port, query):
    params = urllib.parse.urlencode({"query": query, "limit": 100, "since": "15m"})
    _, body = get_json(f"http://127.0.0.1:{port}/loki/api/v1/query_range?{params}")
    lines = []
    for stream in body.get("data", {}).get("result", []):
        for _, line in stream.get("values", []):
            lines.append((stream.get("stream", {}), line))
    return lines


def clickhouse(query):
    out = subprocess.run(["kubectl", "exec", "deploy/clickhouse", "-c", "clickhouse", "--",
                          "clickhouse-client", "--format", "JSONEachRow", "-q", query],
                         capture_output=True, text=True, timeout=60, check=False)
    return [json.loads(line) for line in out.stdout.splitlines() if line.strip()]


def wait_for(label, fetch):
    deadline = time.time() + WAIT_SECONDS
    while time.time() < deadline:
        result = fetch()
        if result:
            return result
        time.sleep(POLL_SECONDS)
    print(f"FAIL: {label} (waited {WAIT_SECONDS} s)")
    return None


def main():
    gateway, gw_port = port_forward("svc/api-gateway", 80)
    loki, loki_port = port_forward("svc/loki", 3100)
    passed = True
    try:
        node = f"ghost-{uuid.uuid4().hex[:8]}"
        status, body = get_json(f"http://127.0.0.1:{gw_port}/v1/nodes/{node}")
        trace_id = body.get("meta", {}).get("trace_id", "")
        if status != 404 or len(trace_id) != 32:
            print(f"FAIL: expected 404 with meta.trace_id, got {status} {body}")
            return 1
        print(f"OK:   failed request {node} → trace_id {trace_id}")

        server_errors = wait_for("eop-server ERROR line in Loki", lambda: [
            line for labels, line in loki_lines(loki_port, f'{{service_name="eop-server"}} | trace_id="{trace_id}"')
            if labels.get("level") == "ERROR"])
        passed &= server_errors is not None
        if server_errors:
            print(f"OK:   Loki has eop-server's ERROR line for the trace: {server_errors[0]!r}")
        gateway_lines = wait_for("api-gateway line in Loki", lambda: loki_lines(
            loki_port, f'{{service_name="api-gateway"}} | trace_id="{trace_id}"'))
        passed &= gateway_lines is not None
        if gateway_lines:
            print(f"OK:   Loki has {len(gateway_lines)} api-gateway line(s) for the same trace")

        spans = wait_for("both services' spans in SigNoz", lambda: (lambda rows: rows if {r["service"] for r in rows} >= {"api-gateway", "eop-server"} else None)(clickhouse(
            "SELECT resource_string_service$$name AS service, name, span_id, parent_span_id, "
            "attributes_string['result'] AS result FROM signoz_traces.distributed_signoz_index_v3 "
            f"WHERE trace_id = '{trace_id}'")))
        passed &= spans is not None
        if spans:
            server = [s for s in spans if s["service"] == "eop-server" and s["name"] == "QUERY_NODE"]
            gateway_ids = {s["span_id"] for s in spans if s["service"] == "api-gateway"}
            ok = bool(server) and server[0]["result"] == "ERROR"
            passed &= ok
            print(("OK:   " if ok else "FAIL: ") + f"SigNoz has eop-server QUERY_NODE with result={server[0]['result'] if server else None}")
            child = bool(server) and server[0]["parent_span_id"] in gateway_ids
            passed &= child
            print(("OK:   " if child else "FAIL: ") + "eop-server's span is a child of the gateway's span (one trace)")
    finally:
        gateway.kill()
        loki.kill()

    if passed:
        print("OK: US-306 AC6 — the failed operation has the same trace_id in SigNoz and Loki")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
