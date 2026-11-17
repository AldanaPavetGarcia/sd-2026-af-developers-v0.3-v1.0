# EOP edge firmware (ZephyrOS)

Firmware of the bunker edge device (US-401, US-402, US-403, ADR-013, ADR-014). It samples the
bunker every second and publishes the telemetry over MQTT with QoS 1, buffering up to 60 seconds of
it while offline; a watchdog restarts a hung thread and a fatal error reboots the device. The squad has no ESP32 board, so it runs
on Zephyr's **`native_sim`** board: a Linux process that uses the host's sockets, so the MQTT
traffic, the broker and everything downstream are real. See [§ Simulation limits](#simulation-limits).

```text
firmware/eop-edge/
├── west.yml               # Zephyr pin (v4.4.2): the only place the version is set
├── Kconfig  prj.conf      # every setting (identity, broker, periods, priorities, stacks)
├── boards/                # per-board settings (native_sim: offloaded sockets, larger stacks)
├── include/  src/         # threads, MQTT, payload, queue, backoff, status + shell, platform
├── fault_inject.conf      # test build with the `eop fault` shell commands (US-403 AC6)
├── tests/unit/            # ztest (twister): backoff, payloads, identity rules, queue, flush, link,
│                          # watchdog, LED patterns
├── tests/integration/     # against a real Mosquitto: US-401, US-402 and (fault build) US-403
└── Dockerfile             # native_sim build in a 50 MB-max image
```

## Threads (ADR-014)

| Thread | Priority | Does |
|---|---|---|
| sensor | 5 | Every `CONFIG_EOP_SAMPLE_PERIOD_MS` (1 s) builds a sample — `message_id` +1, wall-clock timestamp, simulated temperature (bounded random walk), status from the temperature — and puts it in the queue. A full queue loses its **oldest** sample, counted as *dropped*. |
| MQTT | 6 | Owns the broker connection. Takes samples in FIFO order, publishes each with QoS 1 and waits for its PUBACK before the next one. A sample without PUBACK stays pending and goes first after reconnecting. Reconnects with exponential backoff 1, 2, 4, 8, 16, 30 s. Publishes the events and the health report. |
| supervisor | 1 | The watchdog (US-403). Every second (`k_timer`) restarts a thread that has not checked in for 5 s, and picks the LED pattern. |

## Thread resilience (US-403)

The sensor and MQTT threads check in with the watchdog on every loop, and inside every wait that
can last (PUBACK, CONNACK, backoff sleeps are split in 1 s steps), so a healthy thread is never
silent for 5 s.

* **A thread that misses its check-in for 5 s is aborted and created again**, alone: the other
  thread is not touched. A restarted thread keeps its state, which lives in its module and not on
  the aborted stack: the sensor goes on with the same `message_id` sequence, and the MQTT thread
  closes the old socket, reconnects and resends the sample that had no PUBACK. Nothing is lost.
* **Each restart is counted per thread** and a health report with the new `restarts` goes out at
  once (within 10 s, AC7); `eop status` shows them too.
* **A fatal kernel error reboots the device** (`k_sys_fatal_error_handler` → `sys_reboot`). On
  `native_sim` the process starts itself again (`CONFIG_NATIVE_SIM_REBOOT`): new `boot_id`,
  `BOOT` event, `message_id` from 1.
* Limit: a thread aborted while holding a lock would leave it held. The threads never block while
  holding one (ADR-014), so that only happens if a thread hangs inside a critical section.

### LED patterns

The status LED is the devicetree alias `led0`: the emulated GPIO on `native_sim` (shown by
`eop status`), GPIO2 on the ESP32 (`boards/esp32_devkitc_procpu.overlay`; the DevKitC itself has
no user LED, so an LED goes from GPIO2 through a resistor to GND).

| Pattern | Blink | Meaning |
|---|---|---|
| ok | 100 ms on every 2 s | connected, every thread alive |
| offline | 500 ms on, 500 ms off (1 Hz) | no broker connection: samples are being buffered |
| thread error | 100 ms on, 100 ms off (5 Hz), for 10 s after a restart | a thread was restarted by the watchdog |

A thread error hides being offline: a restarted MQTT thread is also briefly disconnected.

### Fault injection (test builds)

`make firmware-fault-build` adds `fault_inject.conf` (`CONFIG_EOP_FAULT_INJECT=y`) and with it:

| Command | Does |
|---|---|
| `eop fault hang sensor` / `eop fault hang mqtt` | that thread stops at its next check-in, as if hung |
| `eop fault crash` | raises a fatal kernel error (`k_oops`) |

```text
uart:~$ eop fault hang mqtt
<wrn> eop_wdt: fault injection: mqtt thread hangs
<err> eop_wdt: mqtt thread missed its check-in for more than 5000 ms; restarting it
<wrn> eop_wdt: mqtt thread restarted (1 restarts since boot)
<wrn> eop_mqtt: restarted; nothing in flight, reconnecting
<inf> eop_mqtt: reconnected: 6 buffered samples delivered
uart:~$ eop fault crash
<err> eop_fatal: fatal kernel error (reason 3): rebooting
native_sim_reboot: Restarting process.
```

The device build never has these commands.

## Offline buffering (US-402)

The queue (`k_msgq`, `CONFIG_EOP_QUEUE_LEN` = 60) is also the offline buffer. While the network or
the broker is gone the MQTT thread stops consuming, and the sensor keeps sampling into it:

* **Up to 60 samples (60 s) are kept.** After reconnecting they are delivered first, oldest first,
  before any new sample, and their `message_id`s stay consecutive.
* **When the buffer is full the oldest sample is discarded** to make room for the newest one: a
  longer outage loses its beginning, not its end. Every discarded sample is counted: `dropped` in
  `eop status` and in the health report (the metric of the Grafana edge panel, US-405).
* **After each reconnection** the device publishes `RECONNECTED` on `…/events` with `delivered`,
  the buffered samples sent; the first connection since boot publishes `BOOT` instead.
* A sample whose PUBACK was lost may reach the broker twice (QoS 1 is at-least-once). The
  ingestor drops the copy by `(node_id, boot_id, message_id)` (US-402 AC7, US-404).

On `native_sim` the link is cut by hand, as the WiFi would be lost on the board:

```text
uart:~$ eop net down
link down: buffering samples
<wrn> eop_mqtt: link down: closing the broker connection, buffering samples
<wrn> eop_mqtt: link down, 1 samples buffered, next attempt in 2 s
uart:~$ eop net up
link up: reconnecting now
<inf> eop_mqtt: connected to 127.0.0.1:1883 as vault-7, publishing to eop/bunker/vault-7/telemetry
<inf> eop_mqtt: reconnected: 7 buffered samples delivered
```

While the link is down the connection attempts keep their backoff (1, 2, 4 … 30 s); bringing it up
wakes the MQTT thread at once, as the board's "network connected" event would.

## Payload

`eop/bunker/{node_id}/telemetry`, one JSON object per sample
([`docs/edge-telemetry-protocol-v1.0.md`](../../docs/edge-telemetry-protocol-v1.0.md) §2.1):

```json
{"node_id":"vault-101","bunker_name":"Vault-101","timestamp":"2026-11-02T14:03:07.120Z",
 "temperature":21.4,"status":"OK","message_id":4182,"boot_id":"9f3c27a1"}
```

`message_id` restarts at 1 at every boot; `boot_id` (random per boot) tells boots apart, so the
ingestor can deduplicate by `(node_id, boot_id, message_id)` (ADR-028).

Also published, with QoS 1 (protocol §2.2, §2.3):

| Topic | When | Payload |
|---|---|---|
| `eop/bunker/{node_id}/events` | first connection (`BOOT`), every reconnection (`RECONNECTED`) | `{"node_id":…,"boot_id":…,"timestamp":…,"event":"RECONNECTED","delivered":37}` |
| `eop/bunker/{node_id}/health` | every `CONFIG_EOP_HEALTH_PERIOD_MS` (10 s) while connected | `{"node_id":…,"boot_id":…,"timestamp":…,"uptime_s":3600,"restarts":{"sensor":0,"mqtt":0},"buffered":0,"dropped":0}` |

`restarts` counts the watchdog restarts of each thread since boot (US-403).

## Configuration

Everything is a Kconfig option set in `prj.conf` (US-401 AC7); nothing is hardcoded in `src/`.

| Option | Default | Meaning |
|---|---|---|
| `CONFIG_EOP_NODE_ID` | `vault-101` | `[a-z0-9-]{1,63}`, not starting with `hpc-engine` |
| `CONFIG_EOP_BUNKER_NAME` | `Vault-101` | `[A-Za-z0-9_.-]{1,63}` |
| `CONFIG_EOP_MQTT_BROKER_HOST` / `_PORT` | `mosquitto` / `1883` | Broker |
| `CONFIG_EOP_MQTT_TOPIC_PREFIX` | `eop/bunker` | Topics are `<prefix>/<node_id>/…` |
| `CONFIG_EOP_SAMPLE_PERIOD_MS` | `1000` | Sampling period |
| `CONFIG_EOP_QUEUE_LEN` | `60` | Queue / offline buffer, in samples |
| `CONFIG_EOP_HEALTH_PERIOD_MS` | `10000` | Health report period |
| `CONFIG_EOP_WDT_TIMEOUT_MS` / `_CHECK_MS` | `5000` / `1000` | Watchdog timeout and supervisor period |
| `CONFIG_EOP_LED_ERROR_HOLD_MS` | `10000` | How long the LED shows a thread error |
| `CONFIG_EOP_BACKOFF_INITIAL_S` / `_MAX_S` | `1` / `30` | Reconnection backoff |
| `CONFIG_EOP_MQTT_KEEPALIVE_S` | `30` | MQTT keep-alive |
| `CONFIG_EOP_SENSOR_PRIORITY` / `_MQTT_PRIORITY` | `5` / `6` | Thread priorities |
| `CONFIG_EOP_SENSOR_STACK_SIZE` / `_MQTT_STACK_SIZE` | `512` / `2048` | ESP32 stacks; `native_sim` raises them in `boards/` |
| `CONFIG_EOP_TEMP_WARN_DC` / `_ALERT_DC` | `300` / `350` | Status thresholds, tenths of °C |

On `native_sim`, `--node-id=`, `--bunker-name=`, `--broker-host=` and `--broker-port=` override the
`prj.conf` values at start, so one image runs any number of devices. An invalid identity or port
stops the firmware at boot with an error instead of publishing wrong data.

## Build, run, test

```bash
make firmware-setup              # once: Zephyr workspace in ~/.cache/eop-zephyr-ws (EOP_ZEPHYR_WS)
make firmware-build              # build/firmware/zephyr/zephyr.exe
docker compose up -d mosquitto
make firmware-run FIRMWARE_ARGS="--node-id=vault-7 --bunker-name=Vault-7"
make firmware-test               # unit tests (ztest, twister)
make firmware-integration-test   # US-401..US-403 against Mosquitto in Docker (also builds the fault build)
make firmware-image              # eop-edge:local
docker compose --profile edge up -d eop-edge
```

Host requirements: `gcc`, `cmake` ≥ 3.20, `ninja`, `python3` with `venv`, `git`, Docker for the
integration tests. The `native_sim` build uses the host compiler; no Zephyr SDK is needed.

### Shell

The device shell is on the process's stdin/stdout (`make firmware-run`, or
`docker attach eop-edge`):

| Command | Does |
|---|---|
| `eop status` | Link and connection state, counters (below) |
| `eop net down` / `eop net up` | `native_sim` only: cut and restore the simulated network link |
| `eop fault …` | test builds only, see [Fault injection](#fault-injection-test-builds) |

```text
uart:~$ eop status
link:           down
connection:     disconnected
last published: 2026-10-26T00:36:58.561Z
messages sent:  5
buffered:       6
dropped:        0 (oldest samples discarded when the buffer is full)
connect failures: 3
reconnections:  0 (last one delivered 0 buffered samples)
restarts:       sensor 0, mqtt 0
led:            offline
```

`connect failures` counts every attempt made while offline, link down included.

## What the tests check

| US-401 | Check | Where |
|---|---|---|
| AC1 | one sample per second, `message_id` +1, timestamps 1 s apart | `tests/integration` |
| AC2 | topic `eop/bunker/{node_id}/telemetry`, QoS 1 | `tests/integration` |
| AC3 | sampling → broker < 100 ms (P99) | `tests/integration` (≈ 33 ms on a laptop) |
| AC4 | JSON fields, types and rules of protocol §2.1 | `tests/unit`, `tests/integration` |
| AC5 | broker down: no crash, 1-2-4-8 s backoff, publishing resumes | `tests/unit`, `tests/integration` |
| AC6 | first sample at the broker < 10 s after start | `tests/integration` (≈ 0.04 s) |
| AC7 | broker host, port and topic prefix from `prj.conf` | `prj.conf`, `Kconfig` |
| AC8 | `eop status` shell command | manual (above) |

| US-402 | Check | Where |
|---|---|---|
| AC1 | samples kept while offline, 60 of them | `tests/unit` (queue), `tests/integration` |
| AC2 | after reconnecting the buffer goes first, FIFO | `tests/integration` |
| AC3 | full buffer discards the oldest; README (above) and `eop status` say so | `tests/unit`, `tests/integration` (75 s offline) |
| AC4 | 30 s offline → every sample arrives, no gaps, no duplicates | `tests/integration` (automated, with `eop net down/up`) |
| AC5 | reconnection backoff 1, 2, 4, 8 … 30 s | US-401 AC5 tests |
| AC6 | `RECONNECTED` with the number delivered | `tests/unit` (flush), `tests/integration` |
| AC7 | `message_id` +1 per sample within a boot | US-401 AC1 test; dedup itself is US-404 |
| AC8 | `dropped` in the health report | `tests/unit`, `tests/integration`; the panel is US-405 |

| US-403 | Check | Where |
|---|---|---|
| AC1 | a hung thread is restarted after 5 s | `tests/unit` (watchdog), `tests/integration/test_resilience.py` |
| AC2 | per-thread restart counter on `…/health` | `tests/integration/test_resilience.py` |
| AC3 | the other thread keeps working: samples keep flowing / no reconnection | `tests/integration/test_resilience.py` |
| AC4 | fatal error → reboot → publishing again | `tests/integration/test_resilience.py` (`native_sim` reboot) |
| AC5 | LED pattern per state, documented above | `tests/unit` (patterns); visual on the board |
| AC6 | injected fault (Kconfig test build) → restart and counter | `tests/integration/test_resilience.py` |
| AC7 | health with the new counter within 10 s of the restart | `tests/integration/test_resilience.py` |

## Simulation limits

What `native_sim` does not prove, and what would close the gap on a board (ADR-014):

| On an ESP32 | Here | Not proven |
|---|---|---|
| WiFi association, link loss and rejoin (`CONFIG_ESP32_WIFI_STA_RECONNECT`) | The host network; link loss is the shell's `eop net down/up` | WiFi driver events and timing; on the board a lost link is seen when the socket fails, and the reconnection waits for the next backoff attempt (up to 30 s) instead of being woken early |
| Boot ROM + WiFi before the first publish | Process start → first PUBACK | Real boot time |
| 520 KB of SRAM | Host memory | Measured on the ESP32 build: 140 KB of DRAM, 64 KB of IRAM ([ESP32 build](#esp32-build)) |
| Wall clock from SNTP (`src/platform_sntp.c`) | Host clock (`src/platform_native_sim.c`) | SNTP on the device |
| Status LED on GPIO2 | Emulated GPIO, pattern shown by `eop status` | The blink itself |
| Reboot by the ROM after `sys_reboot` | The process restarts itself (`execv`) | Boot time after a crash |
| Hardware RNG for `boot_id` | Host clock mixed with the test entropy source (`WARNING: Using a test - not safe - entropy source` at boot is expected) | — |

Thread scheduling, the queue and buffering, backoff, the watchdog and thread restarts, and the
whole MQTT exchange are the real Zephyr code paths.

## ESP32 build

The same sources build for `esp32_devkitc/esp32/procpu`. That build joins a WiFi network
(`src/wifi_link.c`), sets the clock with SNTP (`src/platform_sntp.c`) and resolves the broker
name with DNS (`boards/esp32_devkitc_procpu.conf`). It needs:

1. the Espressif HAL, mbedTLS and esptool: `scripts/zephyr-setup.sh --esp32`;
2. the Zephyr SDK 1.0.1 with the `xtensa-espressif_esp32_zephyr-elf` toolchain;
3. the network name and password in an **untracked** overlay, for example `wifi.local.conf`
   (`*.local.conf` is ignored by git):

   ```text
   CONFIG_EOP_WIFI_SSID="bunker-net"
   CONFIG_EOP_WIFI_PSK="..."
   CONFIG_EOP_MQTT_BROKER_HOST="192.168.1.20"
   ```

```bash
make firmware-build FIRMWARE_BOARD=esp32_devkitc/esp32/procpu ZEPHYR_TOOLCHAIN=zephyr \
     FIRMWARE_BUILD=build/firmware-esp32 \
     ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-1.0.1 EXTRA_CONF_FILE=$PWD/firmware/eop-edge/wifi.local.conf
west flash -d build/firmware-esp32
```

Built on 2026-11-16 without a board (no overlay, so the device would stop at boot asking for
the network name), the image fits with room to spare in the ESP32's memory:

| Region | Used | Size | |
|---|---|---|---|
| DRAM (dram0 + dram1) | 140 KB | 288 KB | 49 % |
| IRAM | 64 KB | 224 KB | 28 % |
| Flash | 577 KB | 4 MB | 14 % |

It has not been flashed or run: there is no board. It is not built in CI either (the SDK and the
HAL would add several minutes to every firmware run); build it by hand when the firmware changes
until a board is available.
