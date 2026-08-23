# HPC Performance Grafana Panel

## Purpose

The HPC Performance Grafana Panel provides a focused performance view for the `hpc-engine`.

It answers the operational question:

> Is the HPC Engine processing graph analysis requests efficiently?

This dashboard is different from the EOP SLO dashboard. The SLO dashboard focuses on general platform health, while this dashboard focuses on graph-processing performance inside the HPC Engine.

## Dashboard location

The dashboard definition is stored as code in the repository:

```text
monitoring/grafana/dashboards/eop-hpc-performance-dashboard.json
```

The dashboard is expected to be loaded by the Grafana provisioning files under:

```text
monitoring/grafana/provisioning/
```

## Dependency on engine telemetry

This dashboard depends on the engine-side telemetry contract introduced by issue #29.

The engine-side response fields include:

```text
processing_time_ms
thread_count
graph_size
executed_algorithms
algorithm_timings_ms
```

The dashboard does not read ACK payloads directly. A metrics exporter or `/metrics` endpoint must convert those runtime values into Prometheus-compatible metrics.

## Panels

| Panel | Purpose |
| --- | --- |
| Graphs Processed per Minute | Shows the number of successful graph analysis requests processed per minute. |
| HPC Processing Latency P50/P95/P99 | Shows percentile latency for complete HPC request processing. |
| Average HPC Processing Time | Shows average processing duration. |
| Per-Algorithm Timing P95 | Shows per-algorithm execution latency. |
| OpenMP Thread Count | Shows the thread count observed by the engine. |
| Graph Size | Shows the average processed graph size in nodes and edges. |
| OpenMP Speedup Ratio | Shows speedup only when a valid baseline metric is available. |
| HPC Error Rate | Shows the percentage of failed HPC requests. |

## Expected datasource

The dashboard expects a Prometheus-compatible datasource with this Grafana UID:

```text
prometheus
```

## Expected metric contract

The dashboard references the following Prometheus metric names:

```text
eop_hpc_requests_total
eop_hpc_processing_duration_seconds_bucket
eop_hpc_processing_duration_seconds_sum
eop_hpc_processing_duration_seconds_count
eop_hpc_algorithm_duration_seconds_bucket
eop_hpc_thread_count
eop_hpc_graph_nodes
eop_hpc_graph_edges
eop_hpc_speedup_ratio
```

These metrics represent the minimum contract needed to visualize the HPC performance requirements from US-206.

## Speedup note

The dashboard includes a speedup panel, but speedup must only be shown when a valid baseline exists.

A valid baseline can be:

- a single-thread execution of the same workload;
- benchmark-derived reference data;
- a stored historical baseline collected under equivalent conditions.

Speedup must not be approximated as `thread_count`.

## Validation

The dashboard JSON contract can be validated without starting Grafana:

```bash
make hpc-dashboard-test
```

The test checks that:

- the dashboard JSON exists;
- the JSON is valid;
- the dashboard has the expected UID, title, tags, and default time range;
- the required HPC performance panels are present;
- the expected Prometheus query tokens are referenced.

## Notes

This dashboard provides the HPC performance view required by US-206.

It does not implement the metrics exporter, the Prometheus `/metrics` endpoint, Docker Compose deployment, or Kubernetes deployment. Those concerns are handled by separate observability infrastructure issues.
