# shimmy — single-host, shared-memory, SPMC messaging library.
#
# The library itself is HEADER-ONLY (an INTERFACE target). Benchmarks and tests
# are COMPILED targets. Dependencies (GoogleTest, Google Benchmark, HdrHistogram)
# are expected to be provided by the environment — the Nix devshell (flake.nix)
# pins them. We deliberately do NOT fetch dependencies at configure time, so that
# benchmark numbers are reproducible against a pinned toolchain.
cmake_minimum_required(VERSION 3.25)

project(shimmy
  VERSION 0.0.0
  DESCRIPTION "Single-host shared-memory SPMC messaging library"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Project-wide settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Export compile_commands.json for clangd / tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SHIMMY_BUILD_TESTS "Build correctness tests" ON)
option(SHIMMY_BUILD_BENCH "Build benchmarks" ON)
option(SHIMMY_ENABLE_TSAN "Build with ThreadSanitizer (tests only)" OFF)
option(SHIMMY_ENABLE_COVERAGE
  "Build tests with LLVM source-based coverage instrumentation (clang only)" OFF)

# ---------------------------------------------------------------------------
# Header-only library target
# ---------------------------------------------------------------------------
add_library(shimmy INTERFACE)
add_library(shimmy::shimmy ALIAS shimmy)
target_include_directories(shimmy INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(shimmy INTERFACE cxx_std_20)

# Warnings applied to OUR compiled targets (not propagated to consumers).
add_library(shimmy_warnings INTERFACE)
target_compile_options(shimmy_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(SHIMMY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(SHIMMY_BUILD_BENCH)
  add_subdirectory(bench)
endif()
