// Bootstrap benchmark: proves the bench toolchain end-to-end —
//   * Google Benchmark  (the microbenchmark driver)
//   * HdrHistogram_c    (latency histogram recording)
//   * the header-only shimmy library
// all compile, link, and run together. The real throughput sweep / latency
// tail / CSV harness with a CI regression gate lands in shimmy-7d8.
#include <shimmy/version.hpp>

#include <benchmark/benchmark.h>
#include <hdr/hdr_histogram.h>

#include <cstdint>
#include <cstdio>

namespace {

// Trivial microbenchmark so `shimmy_bench` produces real timing output.
void BM_VersionNumberPack(benchmark::State& state) {
  for (auto _ : state) {
    auto v = shimmy::version_number;
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_VersionNumberPack);

// Prove HdrHistogram links and records: build a histogram, record values,
// read back a percentile. This is the recording primitive the latency harness
// in shimmy-7d8 will use.
void BM_HdrHistogramRecord(benchmark::State& state) {
  hdr_histogram* hist = nullptr;
  // 1ns .. 1s, 3 significant figures.
  if (hdr_init(1, 1000000000, 3, &hist) != 0) {
    state.SkipWithError("hdr_init failed");
    return;
  }
  std::int64_t value = 1;
  for (auto _ : state) {
    hdr_record_value(hist, value);
    value = (value % 100000) + 1;
    benchmark::DoNotOptimize(hist);
  }
  benchmark::DoNotOptimize(hdr_value_at_percentile(hist, 99.9));
  hdr_close(hist);
}
BENCHMARK(BM_HdrHistogramRecord);

} // namespace

BENCHMARK_MAIN();
