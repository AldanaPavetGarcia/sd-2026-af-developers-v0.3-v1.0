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
  src/platform.c
)

# The board decides where the wall clock and the configuration overrides come from.
if(CONFIG_EOP_CLOCK_SNTP)
  target_sources(app PRIVATE src/platform_sntp.c)
  target_sources_ifdef(CONFIG_WIFI app PRIVATE src/wifi_link.c)
else()
  target_sources(app PRIVATE src/platform_native_sim.c)
endif()
