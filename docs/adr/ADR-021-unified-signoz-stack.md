# ADR-021: Unified Local SigNoz Stack — Observability Runbook

This document describes how to run and validate the local observability stack for US-110: Distributed Traces in SigNoz.

## Unified local stack

The project uses a single `docker-compose.yml` file to start the complete local observability stack.

The stack includes:

- EOP server
- EOP OpenTelemetry Collector
- SigNoz OpenTelemetry Collector
- SigNoz UI/backend
- ClickHouse
- Zookeeper
- SigNoz telemetristore migrator

ClickHouse, Zookeeper and the migrator are internal SigNoz dependencies. They are not EOP application components, but they are required for a reproducible local SigNoz deployment.

## Telemetry flow

```text
EOP server
  -> EOP OTel Collector
  -> SigNoz OTel Collector
  -> ClickHouse
  -> SigNoz UI
```

The EOP server exports traces to the EOP OpenTelemetry Collector:

```text
http://otel-collector:4318/v1/traces
```

The EOP OpenTelemetry Collector forwards traces to the SigNoz OpenTelemetry Collector:

```text
signoz-otel-collector:4317
```

The SigNoz OpenTelemetry Collector stores traces in ClickHouse, and SigNoz displays them in the UI.

## Start the stack

From the repository root:

```bash
docker compose up --build -d
```

Or use the local deployment target:

```bash
make deploy-local
```

## Check service status

```bash
docker compose ps
```

Expected services:

```text
eop-server
eop-otel-collector
eop-signoz
eop-signoz-clickhouse
eop-signoz-otel-collector
eop-signoz-zookeeper-1
```

Expected health status:

```text
eop-server                  healthy
eop-otel-collector          healthy
eop-signoz                  healthy
eop-signoz-clickhouse       healthy
eop-signoz-otel-collector   healthy
eop-signoz-zookeeper-1      healthy
```

## Access SigNoz

SigNoz UI is available at:

```text
http://127.0.0.1:8081
```

Use the Traces Explorer to search for EOP operations such as:

```text
REGISTER
QUERY_NODE
LIST_NODES
```

## Health checks

Run:

```bash
curl -I http://127.0.0.1:8081 || true
curl -i http://127.0.0.1:13133/ || true
curl -i http://127.0.0.1:8123/ping || true
```

Expected results:

- SigNoz UI responds on port `8081`.
- EOP OTel Collector health endpoint responds on port `13133`.
- ClickHouse responds on port `8123`.

## Run US-110 integration test

The integration test assumes that the unified compose stack is already running.

Run:

```bash
SIGNOZ_CLICKHOUSE_HTTP_URL=http://127.0.0.1:8123 make observability-test
```

Expected result:

```text
SUCCESS: exactly one REGISTER span with result=OK was found in SigNoz.

US-110 SigNoz integration test passed.
```

This validates that:

- The EOP server accepts a real `REGISTER` request.
- The server generates a span for the operation.
- The span is exported through the OpenTelemetry pipeline.
- SigNoz stores the span in ClickHouse.
- The span contains the expected `operation.name`, `result` and `node.id` attributes.

## Manual REGISTER validation

Generate a test `REGISTER` operation:

```bash
python3 - <<'PY'
import socket
import time

node_id = f"manual-signoz-test-{int(time.time())}"

with socket.create_connection(("127.0.0.1", 9026), timeout=5) as sock:
    sock.settimeout(5)

    welcome = sock.recv(4096).decode(errors="replace").strip()
    print("WELCOME:", welcome)

    message = (
        f"REGISTER node_id={node_id} "
        "bunker_name=ManualSigNoz "
        "ip_address=10.110.0.4 "
        "capacity=110\n"
    )

    print("SENDING:", message.strip())
    sock.sendall(message.encode())

    response = sock.recv(4096).decode(errors="replace").strip()
    print("RESPONSE:", response)

print("NODE_ID:", node_id)
PY
```

Then open SigNoz:

```text
http://127.0.0.1:8081
```

In Traces Explorer, search for:

```text
REGISTER
```

or for the generated `node_id`.

## Logs and trace correlation

The EOP server writes structured JSON logs to stdout.

Relevant fields include:

- `timestamp`
- `level`
- `trace_id`
- `span_id`
- `node_id`
- `event`
- `message`

Inspect recent REGISTER logs:

```bash
docker compose logs eop-server | grep REGISTER | tail -10
```

The `trace_id` in the log can be used to correlate the server log entry with the trace visible in SigNoz.

## Clean restart validation

To validate the stack from scratch:

```bash
docker compose down --remove-orphans -v
time docker compose up --build -d
docker compose ps
SIGNOZ_CLICKHOUSE_HTTP_URL=http://127.0.0.1:8123 make observability-test
```

This is the recommended final validation flow for US-110.

## Notes

The complete stack is intentionally not expected to run in GitHub Actions because SigNoz and ClickHouse are relatively heavy services.

CI should remain focused on build, unit tests and lightweight validation. The full SigNoz observability validation is intended to run locally with Docker Compose.
