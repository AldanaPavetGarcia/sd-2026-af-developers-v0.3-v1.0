# EOP edge firmware (US-401..US-403, ADR-014). Built with west against the workspace that
# scripts/zephyr-setup.sh creates from west.yml.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(eop_edge LANGUAGES C)

target_include_directories(app PRIVATE include)
target_sources(app PRIVATE
  src/main.c
  src/sensor.c
  src/mqtt_pub.c
  src/telemetry.c
  src/backoff.c
  src/queue.c
  src/flush.c
  src/link.c
  src/status.c
  src/shell.c
  src/watchdog.c
  src/supervisor.c
  src/led.c
  src/led_pattern.c
  src/fatal.c
  src/platform.c
)

# The board decides where the wall clock and the configuration overrides come from.
if(CONFIG_EOP_CLOCK_SNTP)
  target_sources(app PRIVATE src/platform_sntp.c)
  target_sources_ifdef(CONFIG_WIFI app PRIVATE src/wifi_link.c)
else()
  target_sources(app PRIVATE src/platform_native_sim.c)
endif()

# TLS to the broker (tls.conf): the broker's CA is built into the firmware from a file given at
# build time, never committed (scripts/edge-certs.sh creates it).
if(CONFIG_EOP_MQTT_TLS)
  set(EOP_MQTT_CA_CERT "" CACHE FILEPATH "CA certificate (PEM) that signed the broker's certificate")
  if(NOT EXISTS "${EOP_MQTT_CA_CERT}")
    message(FATAL_ERROR "tls.conf needs -DEOP_MQTT_CA_CERT=<path to the broker's ca.crt>")
  endif()
  generate_inc_file_for_target(app ${EOP_MQTT_CA_CERT} ${ZEPHYR_BINARY_DIR}/include/generated/eop_ca_cert.inc)
  target_sources(app PRIVATE src/tls.c)
endif()
