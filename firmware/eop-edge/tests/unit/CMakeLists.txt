# Unit tests of the firmware modules that need no network: backoff, payload encoding,
# identity validation, the telemetry queue, the reconnection flush and the link state. Run with twister on native_sim.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(eop_edge_unit LANGUAGES C)

set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_include_directories(app PRIVATE ${FW}/include)
target_sources(app PRIVATE
  src/test_backoff.c
  src/test_telemetry.c
  src/test_queue.c
  src/test_flush.c
  src/test_link.c
  ${FW}/src/backoff.c
  ${FW}/src/telemetry.c
  ${FW}/src/queue.c
  ${FW}/src/flush.c
  ${FW}/src/link.c
)
