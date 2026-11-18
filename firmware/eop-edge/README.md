# EOP edge firmware (ZephyrOS)

Firmware of the bunker edge device (US-401, ADR-013, ADR-014). It samples the bunker every
second and publishes the telemetry over MQTT with QoS 1. The squad has no ESP32 board, so it runs
on Zephyr's **`native_sim`** board: a Linux process that uses the host's sockets, so the MQTT
traffic, the broker and everything downstream are real. See [§ Simulation limits](#simulation-limits).

```text
firmware/eop-edge/
├── west.yml               # Zephyr pin (v4.4.2): the only place the version is set
├── Kconfig  prj.conf      # every setting (identity, broker, periods, priorities, stacks)
├── boards/                # per-board settings (native_sim: offloaded sockets, larger stacks)
├── include/  src/         # threads, MQTT, payload, queue, backoff, status + shell, platform
├── tests/unit/            # ztest (twister): backoff, payload, identity rules, queue
├── tests/integration/     # against a real Mosquitto: US-401 AC2-AC6
└── Dockerfile             # native_sim build in a 50 MB-max image
```

## Threads (ADR-014)

| Thread | Priority | Does |
|---|---|---|
| sensor | 5 | Every `CONFIG_EOP_SAMPLE_PERIOD_MS` (1 s) builds a sample — `message_id` +1, wall-clock timestamp, simulated temperature (bounded random walk), status from the temperature — and puts it in the queue. A full queue loses its **oldest** sample, counted as *dropped*. |
| MQTT | 6 | Owns the broker connection. Takes samples in FIFO order, publishes each with QoS 1 and waits for its PUBACK before the next one. A sample without PUBACK stays pending and goes first after reconnecting. Reconnects with exponential backoff 1, 2, 4, 8, 16, 30 s. |

The queue (`k_msgq`, `CONFIG_EOP_QUEUE_LEN` = 60) is also the offline buffer: while the broker is
unreachable the MQTT thread stops consuming and up to 60 seconds of samples wait in it.

## Payload

`eop/bunker/{node_id}/telemetry`, one JSON object per sample
([`docs/edge-telemetry-protocol-v1.0.md`](../../docs/edge-telemetry-protocol-v1.0.md) §2.1):

```json
{"node_id":"vault-101","bunker_name":"Vault-101","timestamp":"2026-11-02T14:03:07.120Z",
 "temperature":21.4,"status":"OK","message_id":4182,"boot_id":"9f3c27a1"}
```

`message_id` restarts at 1 at every boot; `boot_id` (random per boot) tells boots apart, so the
ingestor can deduplicate by `(node_id, boot_id, message_id)` (ADR-028).

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
make firmware-integration-test   # US-401 AC2-AC6 against Mosquitto in Docker
make firmware-image              # eop-edge:local
docker compose --profile edge up -d eop-edge
```

Host requirements: `gcc`, `cmake` ≥ 3.20, `ninja`, `python3` with `venv`, `git`, Docker for the
integration tests. The `native_sim` build uses the host compiler; no Zephyr SDK is needed.

### Shell

The device shell is on the process's stdin/stdout (`make firmware-run`, or
`docker attach eop-edge`):

```text
uart:~$ eop status
connection:     connected
last published: 2026-10-23T22:38:53.786Z
messages sent:  2
buffered:       0
dropped:        0 (oldest samples discarded when the buffer is full)
connect failures: 0
```

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

## Simulation limits

What `native_sim` does not prove, and what would close the gap on a board (ADR-014):

| On an ESP32 | Here | Not proven |
|---|---|---|
| WiFi association, link loss | The host network; broker loss is tested, link loss comes with US-402 | WiFi driver events and timing |
| Boot ROM + WiFi before the first publish | Process start → first PUBACK | Real boot time |
| 520 KB of SRAM | Host memory | Measured on the ESP32 build: 141.7 KB of DRAM, 62 KB of IRAM ([ESP32 build](#esp32-build)) |
| Wall clock from SNTP (`src/platform_sntp.c`) | Host clock (`src/platform_native_sim.c`) | SNTP on the device |
| Hardware RNG for `boot_id` | Host clock mixed with the test entropy source (`WARNING: Using a test - not safe - entropy source` at boot is expected) | — |

Thread scheduling, the queue, backoff and the whole MQTT exchange are the real Zephyr code paths.

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

Built on 2026-10-25 without a board (no overlay, so the device would stop at boot asking for
the network name), the image fits with room to spare in the ESP32's memory:

| Region | Used | Size | |
|---|---|---|---|
| DRAM (dram0 + dram1) | 141.7 KB | 288 KB | 49 % |
| IRAM | 62 KB | 224 KB | 28 % |
| Flash | 574 KB | 4 MB | 14 % |

It has not been flashed or run: there is no board. It is not built in CI either (the SDK and the
HAL would add several minutes to every firmware run); build it by hand when the firmware changes
until a board is available.
