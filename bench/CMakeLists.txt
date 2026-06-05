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

# Minimal hot-path microbench for the core ring (shimmy-4d4). The full analytics
# harness is shimmy-7d8.
add_executable(shimmy_ring_bench
  ring_bench.cpp)

target_link_libraries(shimmy_ring_bench PRIVATE
  shimmy::shimmy
  shimmy_warnings
  benchmark::benchmark)

# ---------------------------------------------------------------------------
# Analytics harness (shimmy-7d8): the "data doesn't lie" backbone.
# These are STANDALONE executables (own timing + threading + CSV), NOT Google
# Benchmark driver programs — they need precise control of the producer/consumer
# threads and the timed window, which the GBench loop would obscure. They link
# HdrHistogram (latency distributions) and pthreads (both spawn pinned worker
# threads via std::thread + pthread_setaffinity_np).
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

# LATENCY harness: per-wait-strategy end-to-end p50..p99.99 (HdrHistogram, CSV).
add_executable(shimmy_latency_bench
  latency_bench.cpp)
target_link_libraries(shimmy_latency_bench PRIVATE
  shimmy::shimmy
  shimmy_warnings
  PkgConfig::HDRHISTOGRAM
  Threads::Threads)

# THROUGHPUT harness: single-producer msgs/sec sweep over size x consumers (CSV).
# It reports rates (not distributions) but shares common.hpp, which includes the
# hdr header for the shared Histogram type, so it links HdrHistogram too.
add_executable(shimmy_throughput_bench
  throughput_bench.cpp)
target_link_libraries(shimmy_throughput_bench PRIVATE
  shimmy::shimmy
  shimmy_warnings
  PkgConfig::HDRHISTOGRAM
  Threads::Threads)
