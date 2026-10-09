# EOP Go services

Go module for the EOP services written in Go (ADR-010). Today it contains the **API gateway**
(US-301); the v1.0 edge-ingestor (US-404) will be a second binary in the same module and reuse
`internal/eopclient`.

```text
services/go/
├── api/openapi.yaml         # REST contract (OpenAPI 3.0), embedded in the binary
├── cmd/api-gateway/         # entry point
├── internal/eopclient/      # TCP client of the EOP socket protocol (pool, framing, parsing)
├── internal/api/            # /v1 routes, response envelope, error mapping
├── internal/app/            # wiring, HTTP servers, graceful shutdown
├── internal/config/         # environment configuration
├── internal/telemetry/      # JSON logs, Prometheus metrics, OTel traces
└── test/integration/        # tests against a real eop_server (build tag: integration)
```

## API

| Route | Description |
|---|---|
| `GET /v1/nodes` | Every registered node |
| `GET /v1/nodes/{node_id}` | One node; `404` with `NODE_NOT_FOUND` if unknown, `400` with `INVALID_NODE_ID` if malformed |
| `GET /v1/health` | Readiness: `200` when eop-server answers, `503` otherwise |
| `GET /v1/openapi.json` | The contract |

Successful responses: `{"data": …, "meta": {"timestamp": …, "trace_id": …}}`.
Errors: `{"error": {"code": …, "message": …, "field": …}, "meta": …}`. The full contract is
[`api/openapi.yaml`](api/openapi.yaml); unit tests validate every response against it.

Internal listener (not exposed by the Service): `GET /metrics` (Prometheus) and `GET /livez`
(liveness).

## Configuration

All values come from the environment; the defaults run against a local eop-server.

| Variable | Default | Meaning |
|---|---|---|
| `EOP_GATEWAY_LISTEN_ADDR` | `:8080` | Public API listener |
| `EOP_GATEWAY_METRICS_ADDR` | `:9100` | `/metrics` and `/livez` listener |
| `EOP_SERVER_ADDR` | `127.0.0.1:9026` | eop-server |
| `EOP_SERVER_DIAL_TIMEOUT` | `2s` | Connect + WELCOME banner |
| `EOP_SERVER_REQUEST_TIMEOUT` | `3s` | One command |
| `EOP_SERVER_POOL_IDLE_TIMEOUT` | `20s` | Keep below eop-server's `EOP_IDLE_TIMEOUT` |
| `EOP_SERVER_POOL_SIZE` | `8` | Max open connections to eop-server |
| `EOP_GATEWAY_MAX_LIST_NODES` | `100000` | Largest `LIST_NODES` answer accepted |
| `EOP_GATEWAY_MAX_LINE_BYTES` | `65536` | Longest response line accepted |
| `EOP_GATEWAY_HEALTH_TIMEOUT` | `1s` | Readiness check budget |
| `EOP_GATEWAY_SHUTDOWN_TIMEOUT` | `15s` | Draining on SIGTERM |
| `EOP_GATEWAY_VERSION`, `EOP_GATEWAY_TRACK` | `dev`, `stable` | Labels on metrics and traces (canary, ADR-011) |
| `EOP_TRACING_ENABLED` | `1` | OTLP trace export |
| `OTEL_SERVICE_NAME`, `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` | `api-gateway`, SDK default | Standard OpenTelemetry variables |

## Build and run

Requires Go 1.27+.

```bash
# from the repository root
make gateway-image                       # docker image api-gateway:local (~19 MB)
docker compose up -d eop-server api-gateway
curl -s localhost:8080/v1/nodes

# or without Docker, against a local eop_server
cd services/go && EOP_SERVER_ADDR=127.0.0.1:9026 go run ./cmd/api-gateway
```

Kubernetes: `kubectl apply -k k8s/api-gateway/base` (also part of `make k8s-apply`).

## Tests

```bash
make gateway-test                # go vet + unit tests with the race detector
make gateway-lint                # gofmt + golangci-lint
make gateway-coverage            # fails below 90 % of statements
make gateway-integration-test    # US-301 AC8 against build/dev/eop_server (EOP_SERVER_BIN=… to override)
```
