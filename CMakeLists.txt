cmake_minimum_required(VERSION 3.30)

# Configure vcpkg manifest features before project() call
option(EXODUS_BUILD_TESTS "Build unit tests" ON)
option(EXODUS_BUILD_BENCHMARKS "Build benchmarks" OFF)

if(EXODUS_BUILD_TESTS)
  list(APPEND VCPKG_MANIFEST_FEATURES "tests")
endif()
if(EXODUS_BUILD_BENCHMARKS)
  list(APPEND VCPKG_MANIFEST_FEATURES "benchmarks")
endif()

project(exodus-eop-v LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
enable_testing()

# =============================================================================
# Opciones
# =============================================================================
option(EXODUS_ENABLE_COVERAGE "Enable coverage flags (local only)" OFF)
option(EXODUS_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(EXODUS_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(EXODUS_ENABLE_ASAN AND EXODUS_ENABLE_TSAN)
  message(FATAL_ERROR "EXODUS_ENABLE_ASAN and EXODUS_ENABLE_TSAN cannot be enabled at the same time")
endif()

# =============================================================================
# Flags de compilación
# =============================================================================
add_compile_options(-Wall -Wextra -Wpedantic)

# =============================================================================
# Biblioteca estática
# =============================================================================
add_library(exodus_lib STATIC
  src/exodusBanner.cpp
)

target_include_directories(exodus_lib
  PUBLIC include
)

find_package(opentelemetry-cpp CONFIG REQUIRED)
find_package(prometheus-cpp CONFIG REQUIRED)

# =============================================================================
# Ejecutable principal
# =============================================================================
add_executable(eop_server
  src/app/eopServer.cpp
  lib/src/eventAuditLogger.cpp
  lib/src/systemTimeValidator.cpp
  lib/src/commonProtocol.cpp
  lib/src/configLoader.cpp
  lib/src/traceContext.cpp
)

target_include_directories(eop_server PRIVATE include lib/include)
target_link_libraries(eop_server PRIVATE
  exodus_lib
  opentelemetry-cpp::api
  opentelemetry-cpp::sdk
  opentelemetry-cpp::otlp_http_exporter
  prometheus-cpp::core
  prometheus-cpp::pull
)

# =============================================================================
# EOP Client Library (Static and Shared)
# =============================================================================
add_library(eop_client_static STATIC src/app/eop_client.c)
set_target_properties(eop_client_static PROPERTIES OUTPUT_NAME "eop_client")
target_include_directories(eop_client_static PUBLIC include)

add_library(eop_client_shared SHARED src/app/eop_client.c)
set_target_properties(eop_client_shared PROPERTIES OUTPUT_NAME "eop_client")
target_include_directories(eop_client_shared PUBLIC include)

# Example program
add_executable(connect_basic examples/connect_basic.c)
target_link_libraries(connect_basic PRIVATE eop_client_static)

# C client used by the smoke_analyze_graph_c_client test below (US-201 AC8 / US-203 AC2) --
# drives a real ANALYZE_GRAPH request through libeop, not a raw socket.
add_executable(analyze_graph_c_client tests/smoke/analyze_graph_c_client.c)
target_link_libraries(analyze_graph_c_client PRIVATE eop_client_static)

# =============================================================================
# Tests (GTest)
# =============================================================================
if(EXODUS_BUILD_TESTS)
  find_package(GTest CONFIG REQUIRED)

  add_executable(unit_tests
    tests/unit/exodusBannerTest.cpp
    tests/unit/testConnection.cpp
    tests/unit/commonProtocolTest.cpp
    tests/unit/systemTimeValidatorTest.cpp
    tests/unit/eventAuditLoggerTest.cpp
    tests/unit/configLoaderTest.cpp
    tests/unit/testLoad.cpp
    tests/unit/testClient.cpp
    tests/unit/testClientDisconnect.cpp
    tests/unit/testMetrics.cpp
    tests/unit/traceContextTest.cpp
    src/app/eop_client.c
    src/app/eopServer.cpp
    lib/src/eventAuditLogger.cpp
    lib/src/systemTimeValidator.cpp
    lib/src/commonProtocol.cpp
    lib/src/configLoader.cpp
    lib/src/traceContext.cpp
  )

  target_include_directories(unit_tests PRIVATE include lib/include)
  target_compile_definitions(unit_tests PRIVATE TEST_MODE)

  target_link_libraries(unit_tests PRIVATE
    exodus_lib
    GTest::gtest
    GTest::gtest_main
    opentelemetry-cpp::api
    opentelemetry-cpp::sdk
    opentelemetry-cpp::otlp_http_exporter
    prometheus-cpp::core
    prometheus-cpp::pull
  )

  if(EXODUS_ENABLE_COVERAGE)
    target_compile_options(unit_tests PRIVATE -O0 -g --coverage)
    target_compile_options(exodus_lib PRIVATE -O0 -g --coverage)
    target_link_options(unit_tests PRIVATE --coverage)
  endif()

  if(EXODUS_ENABLE_ASAN)
    target_compile_definitions(unit_tests PRIVATE EXODUS_ENABLE_ASAN_BUILD)
    target_compile_options(unit_tests PRIVATE -g -O1 -fsanitize=address -fno-omit-frame-pointer)
    target_compile_options(exodus_lib PRIVATE -g -O1 -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(unit_tests PRIVATE -fsanitize=address)
  endif()

  if(EXODUS_ENABLE_TSAN)
    target_compile_definitions(unit_tests PRIVATE EXODUS_ENABLE_TSAN_BUILD)
    target_compile_options(unit_tests PRIVATE -g -O1 -fsanitize=thread)
    target_compile_options(exodus_lib PRIVATE -g -O1 -fsanitize=thread)
    target_link_options(unit_tests PRIVATE -fsanitize=thread)
  endif()

  include(GoogleTest)
  gtest_add_tests(TARGET unit_tests)
endif()

# =============================================================================
# Benchmarks (Google Benchmark)
# =============================================================================
if(EXODUS_BUILD_BENCHMARKS)
  find_package(benchmark CONFIG REQUIRED)

  add_executable(benchmarks_run
    benchmarks/bench_exodus_banner.cpp
  )

  target_include_directories(benchmarks_run PRIVATE include)
  target_link_libraries(benchmarks_run PRIVATE
    exodus_lib
    benchmark::benchmark
    benchmark::benchmark_main
  )
endif()

# =============================================================================
# HPC Engine (standalone subprocess for graph analysis)
# =============================================================================
add_subdirectory(hpc-engine)


# =============================================================================
# Smoke tests
# =============================================================================
if(EXODUS_BUILD_TESTS)
  add_test(
    NAME smoke_analyze_graph_real_engine
    COMMAND python3
      ${CMAKE_SOURCE_DIR}/tests/smoke/analyze_graph_real_engine.py
      $<TARGET_FILE:eop_server>
      $<TARGET_FILE:hpc_engine>
  )

  add_test(
    NAME smoke_metrics_endpoint
    COMMAND python3
      ${CMAKE_SOURCE_DIR}/tests/smoke/test_metrics_endpoint.py
      $<TARGET_FILE:eop_server>
  )

  add_test(
    NAME smoke_analyze_graph_offline_online_cycle
    COMMAND python3
      ${CMAKE_SOURCE_DIR}/tests/smoke/analyze_graph_offline_online_cycle.py
      $<TARGET_FILE:eop_server>
      $<TARGET_FILE:hpc_engine>
  )

  add_test(
    NAME smoke_analyze_graph_c_client
    COMMAND python3
      ${CMAKE_SOURCE_DIR}/tests/smoke/analyze_graph_c_client_e2e.py
      $<TARGET_FILE:eop_server>
      $<TARGET_FILE:hpc_engine>
      $<TARGET_FILE:analyze_graph_c_client>
  )

  add_test(
    NAME smoke_v01_protocol_backward_compat
    COMMAND python3
      ${CMAKE_SOURCE_DIR}/tests/smoke/v01_protocol_backward_compat.py
      $<TARGET_FILE:eop_server>
  )

  add_test(
    NAME smoke_text_command_trace_context
    COMMAND python3
      ${CMAKE_SOURCE_DIR}/tests/smoke/text_command_trace_context.py
      $<TARGET_FILE:eop_server>
  )

  add_test(
    NAME smoke_message_framing_split_recv
    COMMAND python3
      ${CMAKE_SOURCE_DIR}/tests/smoke/message_framing_split_recv.py
      $<TARGET_FILE:eop_server>
  )

  add_test(
    NAME smoke_trace_context_propagation
    COMMAND python3
      ${CMAKE_SOURCE_DIR}/tests/smoke/trace_context_propagation.py
      $<TARGET_FILE:eop_server>
      $<TARGET_FILE:hpc_engine>
  )

  add_test(
    NAME smoke_analyze_graph_large_response
    COMMAND python3
      ${CMAKE_SOURCE_DIR}/tests/smoke/analyze_graph_large_response.py
      $<TARGET_FILE:eop_server>
      $<TARGET_FILE:hpc_engine>
  )
endif()