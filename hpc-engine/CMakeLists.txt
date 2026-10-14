cmake_minimum_required(VERSION 3.30)

project(hpc-engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# =============================================================================
# Options
# =============================================================================
option(HPC_BUILD_TESTS "Build HPC engine unit tests" ON)
option(HPC_BUILD_BENCHMARKS "Build HPC engine benchmarks" OFF)

# =============================================================================
# Dependencies
# =============================================================================
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenMP REQUIRED)
find_package(prometheus-cpp CONFIG REQUIRED)
find_package(opentelemetry-cpp CONFIG REQUIRED)

# =============================================================================
# HPC Engine static library (shared between executable and tests)
# =============================================================================
add_library(hpc_engine_lib STATIC
  src/json_parser.cpp
  src/validator.cpp
  src/dijkstra.cpp
  src/connected_components.cpp
  src/centrality.cpp
  src/protocol.cpp
  src/engine.cpp
  src/metrics.cpp
  src/tracing.cpp
)

target_include_directories(hpc_engine_lib PUBLIC include)

target_link_libraries(hpc_engine_lib
  PUBLIC
    nlohmann_json::nlohmann_json
    OpenMP::OpenMP_CXX
    prometheus-cpp::core
    prometheus-cpp::pull
    opentelemetry-cpp::api
    opentelemetry-cpp::sdk
    opentelemetry-cpp::trace
    opentelemetry-cpp::otlp_http_exporter
)

target_compile_options(hpc_engine_lib PRIVATE -Wall -Wextra -Wpedantic)

# =============================================================================
# HPC Engine executable
# =============================================================================
add_executable(hpc_engine src/main.cpp)

target_link_libraries(hpc_engine PRIVATE hpc_engine_lib)

target_compile_options(hpc_engine PRIVATE -Wall -Wextra -Wpedantic)

# =============================================================================
# Sanitizer support (inherited from root project options)
# =============================================================================
if(EXODUS_ENABLE_TSAN)
  target_compile_options(hpc_engine_lib PRIVATE -g -O1 -fsanitize=thread)
  target_link_options(hpc_engine_lib PUBLIC -fsanitize=thread)
  target_compile_options(hpc_engine PRIVATE -g -O1 -fsanitize=thread)
  target_link_options(hpc_engine PRIVATE -fsanitize=thread)
endif()

if(EXODUS_ENABLE_ASAN)
  target_compile_options(hpc_engine_lib PRIVATE -g -O1 -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(hpc_engine_lib PUBLIC -fsanitize=address)
  target_compile_options(hpc_engine PRIVATE -g -O1 -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(hpc_engine PRIVATE -fsanitize=address)
endif()

# =============================================================================
# Tests (GTest)
# =============================================================================
if(HPC_BUILD_TESTS)
  enable_testing()
  find_package(GTest CONFIG REQUIRED)

  add_executable(hpc_tests
    tests/test_graph.cpp
    tests/test_validator.cpp
    tests/test_dijkstra.cpp
    tests/test_connected_components.cpp
    tests/test_centrality.cpp
    tests/test_medium_graph.cpp
    tests/test_protocol.cpp
    tests/test_concurrent_algorithms.cpp
    tests/test_metrics.cpp
    tests/test_engine.cpp
    tests/test_tracing.cpp
  )

  target_link_libraries(hpc_tests PRIVATE
    hpc_engine_lib
    GTest::gtest
    GTest::gtest_main
  )

  target_compile_options(hpc_tests PRIVATE -Wall -Wextra -Wpedantic)

  if(EXODUS_ENABLE_TSAN)
    target_compile_options(hpc_tests PRIVATE -g -O1 -fsanitize=thread)
    target_link_options(hpc_tests PRIVATE -fsanitize=thread)
  endif()

  if(EXODUS_ENABLE_ASAN)
    target_compile_options(hpc_tests PRIVATE -g -O1 -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(hpc_tests PRIVATE -fsanitize=address)
  endif()

  include(GoogleTest)
  gtest_discover_tests(hpc_tests)

  add_test(
    NAME smoke_hpc_metrics_endpoint
    COMMAND python3
      ${CMAKE_CURRENT_SOURCE_DIR}/../tests/smoke/test_hpc_metrics_endpoint.py
      $<TARGET_FILE:hpc_engine>
  )
endif()
