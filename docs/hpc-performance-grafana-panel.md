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
| OpenMP Thread Utilization | Shows how many of the configured `OMP_NUM_THREADS` are actually busy (US-206 AC6) -- see below. |

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
eop_hpc_active_threads
eop_hpc_thread_busy_seconds_total
```

These metrics represent the minimum contract needed to visualize the HPC performance requirements from US-206.

## Speedup note

The dashboard includes a speedup panel, but speedup must only be shown when a valid baseline exists.

A valid baseline can be:

- a single-thread execution of the same workload;
- benchmark-derived reference data;
- a stored historical baseline collected under equivalent conditions.

Speedup must not be approximated as `thread_count`.

## Thread utilization note

`eop_hpc_thread_count` answers "how many threads were configured", not "how many are working".
US-206 AC6 asks for the second question, so the utilization panel plots three series:

- `configured` -- `eop_hpc_thread_count`, the ceiling the other two are read against.
- `active now` -- `eop_hpc_active_threads`, an instantaneous gauge of the workers currently
  inside an algorithm's parallel region.
- `busy (avg)` -- `rate(eop_hpc_thread_busy_seconds_total[...])`, the average number of workers
  busy over the window.

Both are published because neither is sufficient alone. The gauge is the honest "right now"
reading, but a request that starts and finishes between two Prometheus scrapes is invisible to
it, so a busy engine can chart as flat zero. The counter's rate cannot miss that work, because
every worker adds its own elapsed time as it leaves the region -- at the cost of being a
windowed average rather than a live value. Utilization is `busy (avg) / configured`.

The instrumentation lives in `hpc::ActiveThreadScope` (`hpc-engine/include/hpc/metrics.hpp`),
constructed once per worker per parallel region in `centrality.cpp`, `dijkstra.cpp` and
`connected_components.cpp`. Serial stretches of a request -- JSON parsing, validation,
serialization -- are deliberately outside it: counting them as busy threads would report
utilization the engine is not actually achieving.

## Historical evidence: the OpenMP Benchmark Report

**US-206 AC3.** The dashboard shows what the engine is doing *now*. The question it cannot
answer -- "is this number good?" -- is answered by the OpenMP Benchmark Report from US-202:

```text
hpc-engine/benchmarks/BENCHMARK_REPORT.md
```

That report is the historical reference for reading this dashboard, and it is what the panels
above should be compared against:

| Panel | Compare against |
| --- | --- |
| HPC Processing Latency P50/P95/P99 | The report's per-thread-count timings at the closest graph size. A P99 far above them means the engine is degraded, not merely slow. |
| OpenMP Speedup Ratio | The report's measured speedup at 1/2/4/8 threads. The panel's live ratio is only meaningful against the same baseline the report established -- see below. |
| OpenMP Thread Utilization | The report's Amdahl's Law analysis (US-202 AC5), which gives the serial fraction and therefore the utilization that is achievable at all. Utilization below that ceiling is expected, not a fault. |
| Graph Size | The report's "when not to parallelize" conclusion (US-202 AC6): the minimum graph size at which parallelism pays off. Requests below it are expected to show poor speedup. |

The link is not only documentary: the single-thread baseline that the speedup panel and the
`eop-hpc-p99-above-2x-baseline` alert are computed from **is** a number measured by that
report, injected as `EOP_HPC_BASELINE_P99_MS` in `k8s/configmap.yaml`. The rationale for
publishing the ratio only inside a documented graph-size band is in
[`adr/ADR-023-hpc-speedup-baseline.md`](adr/ADR-023-hpc-speedup-baseline.md).

Regenerate the report with `make benchmark` (US-202 AC7). It must be regenerated on a machine
with at least 8 physical cores for its 8-thread figures to mean anything.

## Accessing the dashboard

Access URLs, login, and how to bring the stack up are documented once, in the repo
[`README.md`](../README.md#observability), to avoid keeping two copies in sync. Once you're
looking at Grafana, this dashboard is under folder **EOP** → **EOP HPC Performance Dashboard**.

`hpc-engine` has no docker-compose service — it's a Kubernetes-only workload (see #16) — so
this dashboard only shows real data when the whole stack is deployed there (#43). Its Prometheus
target is discovered dynamically via pod annotations (same unified mechanism `eop-server` uses,
see `k8s/prometheus-configmap.yaml`), so it shows up labeled by the pod's actual IP
(e.g. `10.244.0.5:9101`) rather than a fixed hostname.

**No data in the panels?** The metric families only appear on `/metrics` once hpc-engine has
processed at least one `ANALYZE_GRAPH` request (prometheus-cpp doesn't emit a family with zero
recorded children) — send a real request through `eop-server` first (see
`docs/analyze-graph-protocol-v0.2.md` for the request schema), or run `make k8s-hpc-batch-demo`
for a one-shot batch example. Then check the panel's time range (top-right) covers when that
request happened.

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
