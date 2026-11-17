# EOP Edge Telemetry Dashboard

## Purpose

The edge dashboard (US-405) answers one question for the Vault Operator:

> Does bunker telemetry reach EOP fast enough, completely, and from every device?

The pipeline it watches is ESP32 (or `native_sim`) → MQTT (Mosquitto) → `edge-ingestor` →
`eop-server` (ADR-013, ADR-028). The NFR is **E2E latency P99 < 500 ms**, from the device's
sampling time to eop-server's `ACK TELEMETRY`.

## Location and provisioning

| File | What |
|---|---|
| `monitoring/grafana/dashboards/eop-edge-dashboard.json` | The dashboard, uid `eop-edge`, title "EOP Edge Telemetry" |
| `monitoring/grafana/provisioning/alerting/eop-edge-alerts.yaml` | The P99 alert (US-405 AC4) |
| `k8s/grafana-provisioning-configmap.yaml` | Kubernetes copy of the alert (kept identical, checked by the contract test) |

docker-compose mounts the dashboards directory and the alert file into Grafana; `make k8s-apply`
generates the dashboards ConfigMap from `monitoring/grafana/dashboards/` and the Grafana Deployment
mounts the alert. Grafana: http://localhost:3000 → **EOP → EOP Edge Telemetry**.

## Panels

| Panel | Query | US-405 |
|---|---|---|
| Edge Ingestor Broker Connection | `max(eop_edge_broker_connected)` | AC7 (ingestor side) |
| E2E Latency P99 (ESP32 to eop-server) | `histogram_quantile(0.99, sum by (le) (rate(eop_edge_e2e_latency_seconds_bucket[1m])))`, red at 0.5 s | AC3 |
| Edge Message Loss Rate (buffer overflow) | samples devices dropped (`eop_edge_device_dropped`, US-402 AC8) over dropped + received in the last 5 min | AC3 |
| Edge Devices Connected | devices heard from within 10 s | AC7 |
| Edge Messages per Second | received, forwarded, duplicates, invalid (`eop_edge_messages_*_total`) | AC3 |
| Edge E2E Latency (P50, P99) | the same histogram at 0.50 and 0.99, with the 500 ms line | AC3 |
| Edge Devices (broker connectivity) | per device: connected / disconnected, last seen, seconds since, dropped, buffered, thread restarts | AC7 |
| Device Reconnections, Buffer Deliveries and Retries | `RECONNECTED` and the buffered samples it delivered, `BOOT`, ingestor retries, clock skew | — |
| Per-message traces in SigNoz | how to find each message's trace | AC1, AC2 |

**Connected** means the ingestor received a message from the device (telemetry, event or health)
in the last 10 s, from `eop_edge_device_last_seen_timestamp_seconds`, the same liveness the ingestor
uses for `GET /v1/edge/status` and eop-server uses for the node's `status`.

**Latency** is measured by the ingestor when eop-server's ACK arrives, minus the payload's
`timestamp`. It needs the device and cluster clocks to agree (SNTP on the ESP32, the host clock on
`native_sim`); a negative value is counted in `eop_edge_clock_skew_total` instead (ADR-028 §3).

## Traces (US-405 AC1, AC2)

Each telemetry message is one trace in SigNoz, service `edge-ingestor`:

```text
edge.message   starts at the device's sampled_at        attributes: edge.node_id, edge.boot_id,
 └ edge.ingest starts at the MQTT receipt                 edge.message_id, edge.sampled_at,
    └ TELEMETRY (eop-server, through traceparent=)        edge.received_at, edge.delivered_at,
                                                          edge.e2e_latency_ms
```

The length of `edge.message` is the message's E2E latency. Filter by `edge.node_id` to follow one
device.

## Alert (US-405 AC4)

`eop-edge-e2e-p99` (folder EOP) evaluates the P99 expression every 30 s and fires when it stays
above 0.5 s for more than 1 minute (`for: 1m`). No edge telemetry is no data, not an alert: the
device table shows silent devices.

## make edge-test (US-405 AC6)

```bash
make edge-test                                  # 5 devices x 300 s, about 6 minutes
EDGE_TEST_SECONDS=60 EDGE_TEST_DEVICES=3 make edge-test   # shorter run
```

It starts the compose stack (eop-server, redis, mosquitto, edge-ingestor, prometheus,
api-gateway), runs `edge-sim` with a node_id prefix unique to the run, waits for Prometheus to
scrape, and prints a summary read from Prometheus with the dashboard's own expressions. It fails
when:

* the messages received, forwarded or timed by the ingestor differ from what the simulator
  published (exact counter deltas), or any message is invalid;
* the "Edge Messages per Second" panel, averaged over the run, is more than 10 % away from the real
  rate (`EDGE_TEST_RATE_TOLERANCE`);
* the run's P99 is not below 500 ms;
* the loss-rate panel is not 0 %.

`make edge-load-tool-test` tests the comparison itself without the stack (CI).

## Validation

```bash
make edge-dashboard-test      # dashboard, alert, provisioning; every queried metric is exported by the ingestor
make edge-load-tool-test
```

Both run in CI with the other dashboard contract tests.
