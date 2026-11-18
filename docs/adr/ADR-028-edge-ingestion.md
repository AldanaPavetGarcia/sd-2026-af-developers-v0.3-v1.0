# ADR-028: Edge Ingestion — Delivery, Deduplication and Tracing

**Status:** Accepted (v1.0, issue #66). Implemented by US-404 (#72) and US-405 (#73).

## Context and Problem

Devices publish telemetry to the broker with QoS 1 (ADR-013). US-404 asks that each message reach
`eop-server` through the existing socket protocol, that duplicates seen within 5 minutes are not
forwarded, that nothing is dropped silently when the server is down, that one trace covers the path
from MQTT receipt to the server, and that `GET /v1/edge/status` reports the ingestor. US-405 adds
end-to-end latency (device → server) per message, P99 < 500 ms.

Open questions this ADR settles: how telemetry enters a protocol that only knows `REGISTER` and
`HEARTBEAT` from connected sessions; where duplicates are removed without losing messages; how a
trace exists for a device that cannot export spans; and how the gateway learns the ingestor's
state.

## Decision

### 1. A `TELEMETRY` text command

```
TELEMETRY node_id=vault-101 bunker_name=Vault-101 message_id=4182 boot_id=9f3c27a1 temperature=21.4 status=OK sampled_at=1793628187120 [traceparent=00-…]
→ ACK TELEMETRY node_id=vault-101 message_id=4182
```

* Additive, like ADR-015 and the v0.3 trace key: v0.1 commands and clients are unchanged (addendum
  in ADR-003; full grammar in `docs/edge-telemetry-protocol-v1.0.md`).
* Upserts an **edge node**: `source=edge`, `bunker_name` from the device, `ip_address=0.0.0.0`
  (devices are reached through the broker, never directly), `capacity=0`.
* **Liveness by `last_seen`**, not by a session: an edge node is `ONLINE` while its last telemetry
  arrived within `EOP_EDGE_LIVENESS_TIMEOUT` (default 10 s) and `OFFLINE` after it, evaluated when
  the node is read.
* Telemetry state keeps the sample with the newest `sampled_at`: a buffered sample flushed after a
  reconnection (US-402) updates `last_seen` but not the shown temperature if a newer one is known.
* `LIST_NODES` / `QUERY_NODE` append `source=edge temperature=… device_status=… sampled_at=…` to
  edge nodes only; v0.1 nodes are printed exactly as before, so the v0.1 backward-compatibility
  test stays valid.
* A `node_id` belongs to one source: `TELEMETRY` for a node registered over TCP answers
  `ERROR_DUPLICATE`; reserved engine ids (ADR-022) answer `ERROR_INVALID_REQUEST`.

### 2. At-least-once from the broker, effect once at the server

* The ingestor subscribes to `eop/bunker/+/telemetry` (plus `events` and `health` for metrics) with
  QoS 1, a fixed client id and `clean session = false`, so the broker keeps messages while the
  ingestor is away.
* **Manual acknowledgement:** a message is acknowledged to the broker only after the server's
  `ACK TELEMETRY`, or after it is classified as a duplicate or as permanently invalid. A crash in
  between makes the broker deliver it again.
* **Server unreachable:** the ingestor retries the same message with backoff (100 ms doubling to
  5 s), logging each attempt (US-404 AC5). Messages are processed in order, so later ones wait in
  the broker's queue (persisted, ADR-013) instead of in ingestor memory.
* **Deduplication (US-404 AC3, US-402 AC7):** key `edge:dedup:{node_id}:{boot_id}:{message_id}` in
  Redis, written with a 300 s TTL **after** the server acknowledged the message; a message whose
  key exists is acknowledged to the broker and not forwarded. Writing the key before forwarding
  would turn a crash between the two steps into a lost message. `boot_id` keeps a rebooted device,
  whose `message_id` starts again at 1, from being taken for duplicates of its previous run.
* One ingestor replica processes messages sequentially, so a duplicate cannot be in flight next to
  its original. Scaling out needs MQTT shared subscriptions and an in-flight marker; recorded as a
  follow-up, not needed for v1.0's message rate.

### 3. Tracing and end-to-end latency

* MQTT 3.1.1 has no headers and the device has no span exporter, so the **trace starts in the
  ingestor**: a root span `edge.message` whose start time is the device's `sampled_at`, a child
  `edge.ingest` from broker receipt to the server's ACK, and the server's own span as its child
  through `traceparent=` on the `TELEMETRY` line (US-404 AC4). SigNoz therefore shows one trace per
  message with the three timestamps US-405 AC1 names: sampled (publish), received, delivered.
* **E2E latency** = time the server's ACK arrives at the ingestor − `sampled_at`, recorded in a
  histogram per device (`eop_edge_e2e_latency_seconds`). It depends on device and cluster clocks
  agreeing (SNTP on the ESP32, the host clock on `native_sim`); a negative value is counted as
  clock skew and not recorded as latency.
* Health and event topics feed metrics only: buffer overflows (`dropped`), thread restarts and
  reconnections per device, and last-seen time per device for the connectivity panel.

### 4. `GET /v1/edge/status`

The ingestor writes a snapshot to Redis every second (`edge:status`, TTL 5 s): broker connection
state, messages received and forwarded in the last 60 s, devices with their last-seen time. The
gateway reads it; a missing key means the ingestor is down and the endpoint answers
`"state": "disconnected"`. The gateway does not call the ingestor directly, so it needs no
address, circuit breaker or extra port for it, and the answer is the same whichever gateway replica
serves it.

## Alternatives Considered

| Alternative | Reason rejected |
|---|---|
| Ingestor sends `REGISTER` + `HEARTBEAT` per device | Device liveness would follow the ingestor's TCP sessions, so one ingestor restart would mark every device `OFFLINE`; and there is nowhere to put the temperature |
| Dedup key written before forwarding (`SET NX`) | A crash after the key and before the server's ACK loses the message |
| Deduplicate in `eop-server` | US-404 AC3 puts it in the ingestor; the server would need per-device history |
| Trace context generated on the device | No exporter on the device, so its span could not be reported; MQTT 3.1.1 has no header to carry it |
| MQTT 5 user properties for trace context | Same: nothing on the device to continue; and both client libraries are mature on 3.1.1 |
| Gateway calls an ingestor HTTP endpoint | Another service address and failure mode in the gateway; Redis is already its dependency |

## Consequences

* **Positive:** no telemetry is lost between broker and server; duplicates are harmless; one trace
  per message with real timestamps; the edge appears in `/v1/nodes` without new client code.
* **Negative:** E2E latency is only as good as clock synchronisation. A message that the server
  keeps rejecting for a transient reason holds the queue behind it (by design: order and no loss);
  invalid messages are acknowledged, counted and logged so they cannot block it.

## Sequence

```
device        broker          edge-ingestor                Redis           eop-server
  │ PUBLISH QoS1 ─►│                                                          │
  │ ◄── PUBACK ────│── deliver ──►│ span edge.message (start = sampled_at)    │
  │                │              │ EXISTS dedup key ─────────►│             │
  │                │              │ TELEMETRY … traceparent= ────────────────►│ upsert edge node
  │                │              │ ◄──────────────────────── ACK TELEMETRY ──│
  │                │              │ SET dedup key EX 300 ─────►│             │
  │                │ ◄── PUBACK ──│ record E2E latency, end spans             │
```

## References

* PRD — US-402 AC7, US-404, US-405.
* ADR-003 (v1.0 addendum), ADR-013, ADR-014, ADR-018, ADR-022, ADR-026 (trace key on text commands).
* `docs/edge-telemetry-protocol-v1.0.md`.
