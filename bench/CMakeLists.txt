# Compiled benchmark targets. Link Google Benchmark + HdrHistogram. The full
# analytics harness (throughput sweep, perf counters, CSV out, CI regression
# gate) is built out in shimmy-7d8; this is the bootstrap target proving the
# bench toolchain links and runs end-to-end.
find_package(benchmark REQUIRED)

# HdrHistogram_c ships a CMake package config (hdr_histogram) when built with
# CMake, but the canonical pkg-config name is `hdr_histogram`. Probe both so the
# bootstrap works whether the dep comes from Nix or a system install.
find_package(PkgConfig REQUIRED)
pkg_check_modules(HDRHISTOGRAM REQUIRED IMPORTED_TARGET hdr_histogram)

add_executable(shimmy_bench
  smoke_bench.cpp)

target_link_libraries(shimmy_bench PRIVATE
  shimmy::shimmy
  shimmy_warnings
  benchmark::benchmark
  PkgConfig::HDRHISTOGRAM)
