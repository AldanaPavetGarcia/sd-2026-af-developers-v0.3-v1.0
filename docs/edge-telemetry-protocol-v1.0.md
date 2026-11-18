# Edge Telemetry Protocol (v1.0)

Contract between edge devices, the MQTT broker, the edge ingestor and `eop-server`. Decisions and
their reasons: ADR-013 (MQTT), ADR-014 (firmware threads), ADR-028 (ingestion).

## 1. MQTT

| Item | Value |
|---|---|
| Protocol | MQTT 3.1.1 over TCP |
| QoS | 1 for every topic |
| Device listener | `8883` (TLS) in production, `1883` in local development |
| Ingestor listener | `1883`, cluster-internal |
| Device session | client id = `node_id`, `clean session = false`, keep-alive 30 s |
| Topic prefix | `eop/bunker` (configurable: `CONFIG_EOP_MQTT_TOPIC_PREFIX`) |
| Retained messages | not used |

### Topics

| Topic | Publisher | Payload |
|---|---|---|
| `eop/bunker/{node_id}/telemetry` | device, every second | §2.1 |
| `eop/bunker/{node_id}/events` | device, on events | §2.2 |
| `eop/bunker/{node_id}/health` | device, every 10 s and on every thread restart | §2.3 |

## 2. Payloads (UTF-8 JSON, one object per message)

Common rules:

* `node_id`: `[a-z0-9-]{1,63}`, not starting with `hpc-engine` (reserved, ADR-022); equal to the
  topic segment.
* `bunker_name`: `[A-Za-z0-9_.-]{1,63}` (no spaces: it travels in a `key=value` line).
* `boot_id`: 8 lowercase hex digits, random at each boot.
* `timestamp`: RFC 3339 UTC with milliseconds, from the device clock (SNTP on the ESP32).

A message that breaks these rules is acknowledged by the ingestor, logged and counted as invalid,
never forwarded.

### 2.1 Telemetry

```json
{
  "node_id": "vault-101",
  "bunker_name": "Vault-101",
  "timestamp": "2026-11-02T14:03:07.120Z",
  "temperature": 21.4,
  "status": "OK",
  "message_id": 4182,
  "boot_id": "9f3c27a1"
}
```

| Field | Type | Rule |
|---|---|---|
| `temperature` | number | °C, `-50.0`–`150.0` |
| `status` | string | `OK`, `WARN` or `ALERT` |
| `message_id` | integer | `1`…`2^32-1`, +1 per sample within a boot |

### 2.2 Events

```json
{"node_id": "vault-101", "boot_id": "9f3c27a1", "timestamp": "…", "event": "RECONNECTED", "delivered": 37}
```

`event`: `RECONNECTED` (with `delivered`, the buffered messages sent after reconnecting) or `BOOT`.

### 2.3 Health

```json
{"node_id": "vault-101", "boot_id": "9f3c27a1", "timestamp": "…", "uptime_s": 3600,
 "restarts": {"sensor": 0, "mqtt": 1}, "buffered": 0, "dropped": 0}
```

`restarts` and `dropped` are counters since boot.

## 3. Ingestor → `eop-server`: `TELEMETRY`

Text line over the v0.1 socket protocol (ADR-003, `\n` framing):

```
TELEMETRY node_id=<id> bunker_name=<name> message_id=<n> boot_id=<hex8> temperature=<float> status=<OK|WARN|ALERT> sampled_at=<epoch ms> [traceparent=<W3C>]
```

Keys in this order; `traceparent` optional and last (as in the v0.3 addendum of ADR-003).

| Answer | When |
|---|---|
| `ACK TELEMETRY node_id=<id> message_id=<n>` | Stored (or older than the stored sample: `last_seen` updated, state kept) |
| `ERROR_INVALID_REQUEST desc=<reason>` | Missing or malformed key, reserved `node_id` |
| `ERROR_DUPLICATE desc=<reason>` | `node_id` registered over TCP (`source=tcp`) |
| `ERROR_SERVICE_UNAVAILABLE desc=<reason>` | Registry backend unavailable (ADR-027); the ingestor retries |

### Edge nodes in `LIST_NODES` / `QUERY_NODE`

Edge nodes add keys after the v0.1 ones; TCP nodes are printed unchanged:

```
node_id=vault-101 bunker_name=Vault-101 ip_address=0.0.0.0 capacity=0 status=ONLINE last_seen=1793628187 source=edge temperature=21.4 device_status=OK sampled_at=1793628187120
```

`status` is `ONLINE` while the last telemetry is younger than `EOP_EDGE_LIVENESS_TIMEOUT`
(default 10 s).

## 4. Gateway

`GET /v1/edge/status` → `200`:

```json
{"data": {"state": "connected", "window_seconds": 60, "received": 600, "forwarded": 598,
          "duplicates": 2, "invalid": 0,
          "devices": [{"node_id": "vault-101", "last_seen": "…", "connected": true}]},
 "meta": {"timestamp": "…", "trace_id": "…"}}
```

`state` is `connected`, `reconnecting` or `disconnected` (no snapshot from the ingestor in the last
5 s). Edge nodes in `/v1/nodes` carry a `telemetry` object with `temperature`, `status` and
`sampled_at`.
