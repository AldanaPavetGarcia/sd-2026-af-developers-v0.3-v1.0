# EOP Go services

Go module for the EOP services written in Go (ADR-010). Today it contains the **API gateway**
(US-301..US-303) and **external-stub**, a test double for the external AI and corporate services
(ADR-012); the v1.0 edge-ingestor (US-404) will be another binary in the same module and reuse
`internal/eopclient`.

```text
services/go/
├── api/openapi.yaml         # REST contract (OpenAPI 3.0), embedded in the binary
├── cmd/api-gateway/         # entry point
├── cmd/external-stub/       # fake AI / corporate service with fault injection (ADR-012 §1)
├── internal/eopclient/      # TCP client of the EOP socket protocol (pool, framing, parsing, ANALYZE_GRAPH)
├── internal/analysis/       # graph request validation and error classification
├── internal/jobs/           # asynchronous analysis jobs: Redis store and worker pool
├── internal/breaker/        # circuit breaker with a sliding window, settings file format
├── internal/external/       # external service clients: breaker, timeout, fallback cache, live reload
├── internal/stub/           # the external-stub HTTP handlers
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
| `GET /v1/nodes/{node_id}/insights` | Risk assessment from the AI service (circuit breaker) |
| `GET /v1/nodes/{node_id}/corporate` | Supply and compliance status of the node's bunker from the corporate service (circuit breaker) |
| `POST /v1/graph/analyze` | Graph analysis: `200` with the result for graphs under 1,000 nodes, `202` with a `job_id` above |
| `GET /v1/graph/jobs` | Recent jobs, newest first (`?limit=1..200`, default 50), without results |
| `GET /v1/graph/jobs/{job_id}` | Job state and, once succeeded, its result; kept 5 minutes after it finishes |
| `GET /v1/health` | Readiness: `200` when eop-server answers (`status: degraded` if Redis is down), `503` otherwise |
| `GET /v1/openapi.json` | The contract |

Successful responses: `{"data": …, "meta": {"timestamp": …, "trace_id": …}}`.
Errors: `{"error": {"code": …, "message": …, "field": …}, "meta": …}`. The full contract is
[`api/openapi.yaml`](api/openapi.yaml); unit tests validate every response against it.
Every response also carries `X-EOP-Version` and `X-EOP-Track`, taken from the pod's `version`
and `track` labels, so a client or the load test can tell which deployment answered during a
rollout (ADR-011).

Analysis requests are validated in the gateway first (`400` naming the offending field, e.g.
`edges[3].weight`), so an invalid graph never occupies the engine. An analysis that cannot run
because no engine is registered, or the engine timed out, answers `503 HPC_UNAVAILABLE`; for a
large graph this is checked before the job is accepted. Jobs are stored in Redis so any replica
answers a poll; they run in the pod that accepted them, and a job whose pod is stopped is
recorded as failed with `GATEWAY_SHUTDOWN` (or, if the pod died, expires after
`EOP_GATEWAY_JOB_MAX_AGE`).

External services sit behind one circuit breaker each (US-303, ADR-012): the circuit opens after
5 consecutive failures or more than 50 % errors in 10 s (with at least 10 calls), and probes again
after 30 s. While a service fails or its circuit is open, the gateway answers with the last good
data for that item, marked `meta.stale: true` and `meta.stale_since`, or with
`503 SERVICE_DEGRADED` and `Retry-After` when it has none; never with a `500`. Thresholds live in
`k8s/api-gateway/base/common/breaker-configmap.yaml`; the gateway re-reads the mounted file every 10 s
and applies a change once two consecutive reads agree (an empty or half-written file is never
applied), so editing the ConfigMap applies without a restart (kubelet takes up to about a minute
to update the file; 26 s on minikube). State, transitions and fallbacks are Prometheus metrics
(`eop_gateway_circuit_state`, `eop_circuit_breaker_state_transitions_total`,
`eop_gateway_fallback_responses_total`) shown in the SLO dashboard.

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
| `EOP_REDIS_ADDR`, `EOP_REDIS_DB` | `127.0.0.1:6379`, `0` | Job store |
| `EOP_GATEWAY_ASYNC_MIN_NODES` | `1000` | Graph size from which an analysis becomes a job |
| `EOP_GATEWAY_SYNC_ANALYZE_TIMEOUT` | `5s` | Synchronous analysis budget (`504` beyond it) |
| `EOP_SERVER_ANALYZE_TIMEOUT` | `65s` | One `ANALYZE_GRAPH` exchange; keep above eop-server's `EOP_HPC_TIMEOUT_MS` |
| `EOP_GATEWAY_MAX_BODY_BYTES` | `33554432` | Largest request body (`413` beyond it) |
| `EOP_GATEWAY_MAX_ANALYZE_RESPONSE_BYTES` | `67108864` | Largest engine answer accepted |
| `EOP_GATEWAY_JOB_WORKERS`, `EOP_GATEWAY_JOB_QUEUE` | `1`, `16` | Jobs running / waiting per pod (`429` when full) |
| `EOP_GATEWAY_JOB_RESULT_TTL` | `5m` | How long a finished job stays readable |
| `EOP_GATEWAY_JOB_MAX_AGE` | `30m` | Expiry of a job that never finished |
| `EOP_AI_SERVICE_URL`, `EOP_CORPORATE_SERVICE_URL` | `http://127.0.0.1:8091`, `http://127.0.0.1:8092` | External services |
| `EOP_GATEWAY_BREAKER_SETTINGS` | empty (ADR-012 defaults) | Breaker settings file (mounted ConfigMap) |
| `EOP_GATEWAY_BREAKER_RELOAD_INTERVAL` | `10s` | How often that file is re-read |
| `EOP_GATEWAY_EXTERNAL_CACHE_SIZE` | `1000` | Fallback cache entries per external service |
| `EOP_GATEWAY_VERSION`, `EOP_GATEWAY_TRACK` | `dev`, `stable` | Labels on metrics and traces (canary, ADR-011) |
| `EOP_TRACING_ENABLED` | `1` | OTLP trace export |
| `OTEL_SERVICE_NAME`, `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` | `api-gateway`, SDK default | Standard OpenTelemetry variables |

## Build and run

Requires Go 1.27+.

```bash
# from the repository root
make gateway-image                       # docker image api-gateway:local (~19 MB)
docker compose up -d eop-server redis ai-service corporate-service api-gateway
curl -s localhost:8080/v1/nodes

# or without Docker, against a local eop_server
cd services/go && EOP_SERVER_ADDR=127.0.0.1:9026 go run ./cmd/api-gateway
```

Kubernetes: `kubectl apply -f k8s/redis/ -f k8s/external-stubs/ && kubectl apply -k
k8s/api-gateway/overlays/canary` (also part of `make k8s-apply`). `make gateway-image` builds
both images, `api-gateway:local` and `external-stub:local`.

`k8s/api-gateway/base` holds the shared configuration and Service (`common/`) and one
Deployment (`deployment/`); the overlays instantiate that Deployment once per track or slot
(ADR-011). `overlays/canary` runs `api-gateway-stable` and `api-gateway-canary` behind the
Ingress host `api.eop.local`; the canary procedure (`make canary-deploy`, `canary-rollout
WEIGHT=N`, `canary-rollback`, `canary-promote`) is in [docs/RUNBOOK.md](../../docs/RUNBOOK.md),
and `make gateway-load-test` keeps load on the Ingress while it runs.

### Circuit breaker demo (US-303 AC7)

```bash
kubectl port-forward svc/api-gateway 8080:80 &
curl -s localhost:8080/v1/nodes/<node_id>/insights          # fresh data, caches it
kubectl scale deployment/ai-service --replicas=0            # the external service goes down
for i in 1 2 3 4 5; do curl -s -o /dev/null -w "%{http_code} " localhost:8080/v1/nodes/<node_id>/insights; done
# Grafana → EOP SLO Dashboard → "External Service Circuit Breakers": ai = Open
curl -s localhost:8080/v1/nodes/<node_id>/insights          # 200, meta.stale: true (fallback)
curl -si localhost:8080/v1/nodes/<other>/insights           # 503 SERVICE_DEGRADED, Retry-After
kubectl scale deployment/ai-service --replicas=1            # the service recovers
sleep 30; curl -s localhost:8080/v1/nodes/<node_id>/insights  # half-open probe succeeds → Closed, fresh data
```

Failures can also be injected without stopping the stub: `POST /admin/fault` on the stub with
`{"mode": "error"}`, `{"mode": "slow"}` or `{"mode": "ok"}`.

## Tests

```bash
make gateway-test                # go vet + unit tests with the race detector
make gateway-lint                # gofmt + golangci-lint
make gateway-coverage            # fails below 90 % of statements
make gateway-integration-test    # US-301/US-302 AC8 and US-303 AC7 against build/dev/eop_server and hpc_engine
                                 # (EOP_SERVER_BIN=… EOP_HPC_ENGINE_BIN=… to override)
```
