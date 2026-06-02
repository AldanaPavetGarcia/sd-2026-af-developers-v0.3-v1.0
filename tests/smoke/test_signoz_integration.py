#!/usr/bin/env python3
"""
Local integration test for US-110: Distributed Traces in SigNoz.

This test assumes the unified docker compose stack is already running.
It does not start Docker Compose and does not build images.

Expected environment before running:

1. The unified docker compose stack is running.
2. The EOP server is reachable from the host at 127.0.0.1:9026.
3. The EOP OTel Collector is running and exporting to the SigNoz OTel Collector.
4. ClickHouse HTTP is reachable from the host at SIGNOZ_CLICKHOUSE_HTTP_URL.

Typical local flow:

    docker compose up --build -d

    SIGNOZ_CLICKHOUSE_HTTP_URL=http://127.0.0.1:8123 \
    make observability-test

SigNoz UI:

    http://127.0.0.1:8081
"""

import os
import socket
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


EOP_HOST = os.getenv("EOP_HOST", "127.0.0.1")
EOP_PORT = int(os.getenv("EOP_PORT", "9026"))
CLICKHOUSE_HTTP_URL = os.getenv("SIGNOZ_CLICKHOUSE_HTTP_URL", "http://127.0.0.1:8123")
POLL_TIMEOUT_SECONDS = int(os.getenv("SIGNOZ_QUERY_TIMEOUT_SECONDS", "60"))
POLL_INTERVAL_SECONDS = float(os.getenv("SIGNOZ_QUERY_INTERVAL_SECONDS", "5"))


def fail(message: str) -> None:
    print(f"ERROR: {message}")
    sys.exit(1)


def wait_tcp(host: str, port: int, timeout_seconds: int = 30) -> None:
    deadline = time.time() + timeout_seconds

    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=2):
                return
        except OSError:
            time.sleep(1)

    fail(f"TCP endpoint {host}:{port} did not become reachable within {timeout_seconds}s.")


def sql_quote(value: str) -> str:
    return value.replace("\\", "\\\\").replace("'", "\\'")


def query_clickhouse_http(sql: str) -> str:
    encoded = urllib.parse.quote_plus(sql)
    url = f"{CLICKHOUSE_HTTP_URL}/?query={encoded}"

    try:
        with urllib.request.urlopen(url, timeout=10) as response:
            return response.read().decode().strip()
    except urllib.error.URLError as exc:
        raise RuntimeError(f"ClickHouse HTTP query failed against {CLICKHOUSE_HTTP_URL}: {exc}") from exc


def send_register(node_id: str) -> None:
    print(f"Sending REGISTER to EOP server at {EOP_HOST}:{EOP_PORT} with node_id={node_id}...")

    try:
        with socket.create_connection((EOP_HOST, EOP_PORT), timeout=10) as sock:
            sock.settimeout(10)

            welcome = sock.recv(4096).decode(errors="replace").strip()
            print(f"Server welcome: {welcome}")

            message = (
                f"REGISTER node_id={node_id} "
                "bunker_name=US110Integration "
                "ip_address=10.110.0.1 "
                "capacity=110\n"
            )

            sock.sendall(message.encode())
            response = sock.recv(4096).decode(errors="replace").strip()
            print(f"REGISTER response: {response}")

            if "ACK" not in response:
                fail(f"REGISTER did not return ACK. Response was: {response}")

    except OSError as exc:
        fail(f"Could not communicate with EOP server: {exc}")


def find_register_span_count(node_id: str) -> str:
    escaped_node_id = sql_quote(node_id)

    query = f"""
        SELECT count()
        FROM signoz_traces.distributed_signoz_index_v3
        WHERE name = 'REGISTER'
          AND attributes_string['operation.name'] = 'REGISTER'
          AND attributes_string['result'] = 'OK'
          AND attributes_string['node.id'] = '{escaped_node_id}'
    """

    return query_clickhouse_http(query)


def wait_for_register_span(node_id: str) -> None:
    print("Waiting for SigNoz ingestion and querying REGISTER span...")

    deadline = time.time() + POLL_TIMEOUT_SECONDS
    last_count = ""

    while time.time() < deadline:
        try:
            last_count = find_register_span_count(node_id)
            print(f"REGISTER span count for node_id={node_id}: {last_count}")

            if last_count == "1":
                print("SUCCESS: exactly one REGISTER span with result=OK was found in SigNoz.")
                return

        except RuntimeError as exc:
            print(f"Query not ready yet: {exc}")

        time.sleep(POLL_INTERVAL_SECONDS)

    fail(
        "Expected exactly one REGISTER span in SigNoz, "
        f"but final count was '{last_count}'."
    )


def main() -> None:
    node_id = f"us110-register-{int(time.time())}-{os.getpid()}"

    print("=== US-110 SigNoz integration test ===")
    print("This test does not start Docker Compose and does not build images.")
    print(f"EOP endpoint: {EOP_HOST}:{EOP_PORT}")
    print(f"ClickHouse HTTP endpoint: {CLICKHOUSE_HTTP_URL}")
    print(f"Generated node_id: {node_id}")

    print("\n1. Checking EOP server TCP endpoint...")
    wait_tcp(EOP_HOST, EOP_PORT, timeout_seconds=30)

    print("\n2. Checking ClickHouse HTTP endpoint...")
    try:
        query_clickhouse_http("SELECT 1")
    except RuntimeError as exc:
        fail(
            f"{exc}\n"
            "Start the unified docker compose stack and expose ClickHouse HTTP, or set "
            "SIGNOZ_CLICKHOUSE_HTTP_URL."
        )

    print("\n3. Sending REGISTER operation...")
    send_register(node_id)

    print("\n4. Verifying REGISTER span...")
    wait_for_register_span(node_id)

    print("\nUS-110 SigNoz integration test passed.")


if __name__ == "__main__":
    main()
