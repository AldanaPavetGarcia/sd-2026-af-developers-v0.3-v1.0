cmake_minimum_required(VERSION 3.30)
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
option(EXODUS_BUILD_TESTS "Build unit tests" ON)
option(EXODUS_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(EXODUS_ENABLE_COVERAGE "Enable coverage flags (local only)" OFF)
option(EXODUS_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

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

# =============================================================================
# Ejecutable principal
# =============================================================================
add_executable(eop_server
  src/app/eopServer.cpp
  lib/src/eventAuditLogger.cpp
  lib/src/systemTimeValidator.cpp
  lib/src/commonProtocol.cpp
  lib/src/configLoader.cpp
)

target_include_directories(eop_server PRIVATE include lib/include)
target_link_libraries(eop_server PRIVATE exodus_lib)

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
    src/app/eop_client.c
    src/app/eopServer.cpp
    lib/src/eventAuditLogger.cpp
    lib/src/systemTimeValidator.cpp
    lib/src/commonProtocol.cpp
    lib/src/configLoader.cpp
  )

  target_include_directories(unit_tests PRIVATE include lib/include)
  target_compile_definitions(unit_tests PRIVATE TEST_MODE)

  target_link_libraries(unit_tests PRIVATE
    exodus_lib
    GTest::gtest
    GTest::gtest_main
  )

  if(EXODUS_ENABLE_COVERAGE)
    target_compile_options(unit_tests PRIVATE -O0 -g --coverage)
    target_compile_options(exodus_lib PRIVATE -O0 -g --coverage)
    target_link_options(unit_tests PRIVATE --coverage)
  endif()

  if(EXODUS_ENABLE_TSAN)
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
