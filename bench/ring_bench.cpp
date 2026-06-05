// SPDX-License-Identifier: Apache-2.0
//
// Minimal hot-path microbenchmarks for the core ring (shimmy-4d4). The full
// throughput sweep / latency-tail / CSV harness with a CI regression gate is a
// SEPARATE bead (shimmy-7d8); this only sanity-checks that publish + zero-copy
// read are allocation-free and tight, so regressions there are caught early.
#include <shimmy/ring.hpp>

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstring>

namespace {

// Single-producer publish into an Overwrite ring (no consumer): measures the
// raw publish cost — two-phase stamp + memcpy, no allocation, no syscall.
void BM_Publish64(benchmark::State& state) {
  shimmy::Ring<64, 1024, shimmy::Overwrite> ring;
  alignas(64) std::uint8_t msg[64];
  std::memset(msg, 0xAB, sizeof(msg));
  for (auto _ : state) {
    ring.publish(msg, sizeof(msg));
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() * 64);
}
BENCHMARK(BM_Publish64);

// Ping-pong: publish one, zero-copy read + validate it, in lockstep. Measures
// the combined producer publish + consumer acquire + span + validate path with
// no contention (single thread), i.e. the per-message instruction floor.
void BM_PublishReadValidate64(benchmark::State& state) {
  using R = shimmy::Ring<64, 1024, shimmy::Overwrite>;
  R ring;
  shimmy::Consumer<R> c(ring);
  alignas(64) std::uint8_t msg[64];
  std::memset(msg, 0xCD, sizeof(msg));
  for (auto _ : state) {
    ring.publish(msg, sizeof(msg));
    auto view = c.read();
    benchmark::DoNotOptimize(view.data());
    bool ok = c.validate();
    benchmark::DoNotOptimize(ok);
    c.commit();
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PublishReadValidate64);

} // namespace

BENCHMARK_MAIN();
