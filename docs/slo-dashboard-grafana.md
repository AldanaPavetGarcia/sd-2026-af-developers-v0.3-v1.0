# EOP SLO Dashboard in Grafana

## Purpose

The EOP SLO dashboard provides a system-level health view for the Exodus Ops Platform.

It answers the operational question:

> Is the EOP system healthy and operating within the expected service levels?

This dashboard is different from the HPC Engine performance dashboard. The SLO dashboard focuses on the general health of the platform, while the HPC dashboard focuses on graph-processing performance inside `hpc-engine`.

## Dashboard location

The dashboard definition is stored as code in the repository:

```text
monitoring/grafana/dashboards/eop-slo-dashboard.json
```

Manual-only dashboard configuration through the Grafana UI is not accepted for production use. The JSON file must remain versioned and reviewable.

## Default view

The dashboard uses a default time window of:

```text
Last 24 hours
```

It refreshes every:

```text
15 seconds
```

## Panels

| Panel | Purpose |
| --- | --- |
| Service Availability | Shows whether the selected EOP service is available. |
| Requests per Second | Shows request throughput. |
| P99 Request Latency | Shows the slowest observed request behavior using the P99 percentile. |
| Error Rate | Shows the percentage of failed requests. |
| Error Budget Remaining | Estimates remaining error budget against the 99.9% availability target. |
| Average Request Latency | Shows average response time. |
| Failed Requests | Shows failed request rate grouped by service. |

## Expected datasource

The dashboard expects a Prometheus-compatible datasource with this Grafana UID:

```text
prometheus
```

The Prometheus datasource must expose or receive the metrics needed by the dashboard queries.

## Expected metric contract

The dashboard references the following metric names:

```text
up
eop_requests_total
eop_request_duration_seconds_bucket
eop_request_duration_seconds_sum
eop_request_duration_seconds_count
```

These metrics represent the minimum system-health contract required for SLO visualization.

## Validation

The dashboard JSON contract can be validated without starting Grafana:

```bash
make slo-dashboard-test
```

The test checks that:

- the dashboard JSON exists;
- the JSON is valid;
- the dashboard has the expected UID, title, tags, and default time range;
- the required panels are present;
- the expected Prometheus query tokens are referenced.

## Notes

This dashboard provides the general EOP health view required by ADR-012 / US-205.

It does not implement HPC-specific panels such as per-algorithm execution time, graph size, thread count, or OpenMP speedup. Those belong to the separate HPC Performance Grafana Panel.