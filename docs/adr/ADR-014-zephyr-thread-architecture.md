# ADR-014: ZephyrOS Thread Architecture

**Status:** Accepted (v1.0, issue #66). Adapted from the PRD reference ADR-014; implemented by
US-401 (#68), US-402 (#69) and US-403 (#70).

## Context and Problem

The firmware samples a sensor every second and publishes over MQTT (ADR-013). The two activities
fail differently — a slow or failed publish must not delay sampling — and the device has to
recover from a stuck activity without anyone touching it (US-403). ZephyrOS offers preemptive
threads, message queues, timers and a shell; how the firmware uses them decides reliability,
memory use and how failures are isolated.

## Decision

Two application threads, a supervising watchdog thread and one message queue between them:

```
 ┌──────────────── ZephyrOS application ─────────────────┐
 │  sensor thread (prio 5)  ── k_msgq (60) ──►  MQTT thread (prio 6) ──► broker
 │  period 1 s                                  connect / publish QoS 1 / backoff
 │          ▲                                          ▲
 │          └──── watchdog thread (prio 1), k_timer 5 s ┘   restarts a thread that missed its check-in
 └──────────────────────────────────────────────────────────┘
```

* **Sensor thread** (priority 5): every 1 s builds a sample (`node_id`, simulated temperature,
  status, `message_id`, `boot_id`, timestamp) and puts it in the queue. If the queue is full it
  takes the oldest entry out first, so the newest data is kept, and counts the discard (US-402
  AC3, AC8).
* **MQTT thread** (priority 6, below sampling): owns the broker connection. Takes samples from the
  queue in FIFO order and publishes them with QoS 1. A sample whose publish has not been
  acknowledged is kept in a one-entry *pending* slot and sent again first after reconnecting, so
  nothing taken out of the queue is lost and order is kept (US-402 AC2). Reconnects with
  exponential backoff 1, 2, 4, 8 … 30 s (US-401 AC5, US-402 AC5) and publishes `RECONNECTED` with
  the number of buffered messages delivered (US-402 AC6).
* **Queue:** `K_MSGQ_DEFINE(telemetry_queue, sizeof(struct eop_telemetry), 60, 4)` — 60 entries,
  60 seconds of telemetry at one per second. It is the offline buffer: while the broker is
  unreachable the MQTT thread stops consuming and samples accumulate in it.
* **Watchdog thread** (priority 1, highest in the application): a 5 s `k_timer`; each thread calls
  `watchdog_checkin(id)` at the top of its loop. A thread that missed its check-in is aborted and
  created again with its initial state; its restart counter is incremented and published to
  `…/health` (US-403 AC1–AC3, AC7). Threads never hold a lock across a blocking call, so aborting
  one cannot leave a mutex held.
* **Fatal kernel errors:** `k_sys_fatal_error_handler` reboots the device (US-403 AC4).
* **Configuration:** priorities, stack sizes, sampling period, queue length, broker host, port,
  topic prefix, `node_id` and `bunker_name` are Kconfig options set in `prj.conf` (US-401 AC7).
  Fault injection for tests is a Kconfig option too (`EOP_FAULT_INJECT_SENSOR`,
  `EOP_FAULT_INJECT_MQTT`, US-403 AC6), off in normal builds.
* **Shell:** `eop status` prints connection state, last published timestamp, messages sent,
  buffered and dropped (US-401 AC8, US-402 AC3).
* **Stack sizes:** the PRD's 512 bytes (sensor) and 2048 bytes (MQTT) are the ESP32 defaults.
  `native_sim` runs the same threads on a host stack frame layout that needs more, so its board
  configuration raises them; the values are in the board `.conf` files, not in code.

### Running without an ESP32 (`native_sim`)

There is no board (ADR-013), so the firmware runs on `native_sim/native/64`:

| ESP32 behaviour | On `native_sim` | What it does not prove |
|---|---|---|
| WiFi link lost | Shell `eop net down` / `eop net up`: the MQTT thread closes its socket and refuses to reconnect while the link is down | The real WiFi driver's events and timing |
| LED patterns (US-403 AC5) | `led0` on the GPIO emulator; every pattern change is also logged | Visible light |
| Reboot on fatal error | The process exits with a distinct code and its supervisor (container `restartPolicy`) starts it again | ROM boot time |
| Boot in < 10 s (US-401 AC6) | Measured from process start to first PUBACK | ESP32 boot ROM and WiFi association time |
| RAM budget | Not enforced (host memory) | The 520 KB limit — checked by building for `esp32_devkitc` and reading the RAM report |

Thread scheduling, the queue, the watchdog, backoff, buffering and the MQTT exchange are the real
Zephyr code paths in both builds.

## Alternatives Considered

| Alternative | Reason rejected |
|---|---|
| One thread, sample then publish | A network timeout delays the next sample; sampling period depends on the network |
| Sensor thread publishes directly | Same coupling when publish blocks, and no buffer: samples are lost while offline |
| Zephyr asynchronous MQTT in one thread | Removes a thread but needs an event-driven state machine for connect, publish, PUBACK and backoff; harder to get right and to restart in isolation |
| Hardware watchdog (`wdt`) only | Resets the whole device; US-403 AC3 asks that one thread restarts without interrupting the other. The hardware watchdog stays as the last resort behind the watchdog thread on the ESP32 build |

## Consequences

* **Positive:** sampling never waits for the network; the queue is the offline buffer with no
  extra data structure; a stuck thread is restarted alone; every tunable is in `prj.conf`.
* **Negative:** dropping the oldest sample when full is a deliberate loss (counted and visible in
  Grafana, US-402 AC8). Aborting and recreating a thread loses its in-flight state; the MQTT thread
  rebuilds its connection after a restart. On `native_sim`, WiFi, LED and reboot are emulated as
  described above.

## References

* PRD — Milestone 4, US-401, US-402, US-403; reference ADR-014.
* `ADR-013-edge-protocol-mqtt.md` — protocol, topics, payload.
* `ADR-028-edge-ingestion.md` — what happens to the messages after the broker.
