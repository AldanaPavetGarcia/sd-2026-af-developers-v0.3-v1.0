# HPC Engine Prometheus Metrics

## Purpose

The HPC Engine exposes a Prometheus-compatible `/metrics` endpoint so the versioned Grafana dashboard in `monitoring/grafana/dashboards/eop-hpc-performance-dashboard.json` can display live HPC performance data.

This completes the export layer between the runtime telemetry produced by the HPC Engine and the Prometheus/Grafana queries used by the HPC performance dashboard.

## Runtime mode

The `/metrics` endpoint is started only when `hpc_engine` runs in socket mode.

Socket mode is detected when the process is launched with server connection arguments such as:

```bash
./hpc_engine \
  --server-host eop-server \
  --server-port 9026 \
  --node-id hpc-engine
```

Batch mode continues to read one request from `stdin`, write one response to `stdout`, and exit without starting a long-lived metrics endpoint.

## Metrics port

The default metrics port is:

```text
9101
```

It can be overridden with:

```bash
EOP_HPC_METRICS_PORT=9101
```

In Kubernetes, this is configured through `EOP_HPC_METRICS_PORT` in `k8s/configmap.yaml` and exposed as the `metrics` container port in `k8s/hpc-engine-deployment.yaml`.

## Exported metrics

### `eop_hpc_requests_total`

Counter for processed HPC graph analysis requests.

Labels:

```text
service="hpc-engine"
status="ACK" | "ERROR"
```

### `eop_hpc_processing_duration_seconds`

Histogram for total wall-clock processing duration of successful `ANALYZE_GRAPH` requests.

Labels:

```text
service="hpc-engine"
```

### `eop_hpc_algorithm_duration_seconds`

Histogram for per-algorithm execution duration.

Labels:

```text
service="hpc-engine"
algorithm="shortest_paths" | "connected_components" | "centrality"
```

### `eop_hpc_thread_count`

Gauge with the OpenMP thread count observed by the engine.

Labels:

```text
service="hpc-engine"
```

### `eop_hpc_graph_nodes`

Gauge with the number of nodes in the latest successfully processed graph.

Labels:

```text
service="hpc-engine"
```

### `eop_hpc_graph_edges`

Gauge with the number of edges in the latest successfully processed graph.

Labels:

```text
service="hpc-engine"
```

### `eop_hpc_speedup_ratio`

Gauge for the HPC speedup ratio when a valid single-thread baseline is explicitly available.

This metric must not be approximated from `thread_count`. If no real baseline exists, the engine must not invent a speedup value.

## Relationship with the HPC dashboard

The dashboard in `monitoring/grafana/dashboards/eop-hpc-performance-dashboard.json` expects the `eop_hpc_*` metric names defined above.

This endpoint provides the Prometheus scrape layer required for the dashboard to consume live HPC performance data. The dashboard itself is versioned separately and does not read ACK JSON payloads directly.

## Validation

Run:

```bash
make hpc-metrics-test
```

This executes:

```bash
./build/hpc-engine/hpc_tests --gtest_filter='*HpcMetrics*:*Metrics*'
python3 tests/smoke/test_hpc_metrics_endpoint.py ./build/hpc-engine/hpc_engine
```

The smoke test starts a fake EOP server, launches `hpc_engine` in socket mode, scrapes `/metrics`, and verifies that the Prometheus metric families required by the HPC Grafana dashboard are exposed.

## Review note

This metrics endpoint belongs to the `hpc_engine` process. It does not replace or modify the `eop-server` `/metrics` endpoint; both services expose independent Prometheus metrics contracts.

