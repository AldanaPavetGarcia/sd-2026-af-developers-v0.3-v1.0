cmake_minimum_required(VERSION 3.30)
project(exodus-eop-v LANGUAGES CXX)

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

# =============================================================================
# Flags de compilación
# =============================================================================
add_compile_options(-Wall -Wextra -Werror -Wpedantic)

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
add_executable(exodus_app
  src/app/main.cpp
)

target_include_directories(exodus_app PRIVATE include)
target_link_libraries(exodus_app PRIVATE exodus_lib)

# =============================================================================
# Tests (GTest)
# =============================================================================
if(EXODUS_BUILD_TESTS)
  find_package(GTest CONFIG REQUIRED)

  add_executable(unit_tests
    tests/unit/exodusBanner_test.cpp
  )

  target_include_directories(unit_tests PRIVATE include)
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

  include(GoogleTest)
  gtest_discover_tests(unit_tests)
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
