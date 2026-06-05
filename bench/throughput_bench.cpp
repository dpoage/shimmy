// SPDX-License-Identifier: Apache-2.0
//
// shimmy THROUGHPUT harness (bead shimmy-7d8, DESIGN §5).
//
// Single producer, messages/sec, SWEEPING message size {64B,256B,1KB,4KB} and
// consumer count {0,1,2,4}. The headline gate is >=10M msgs/sec @64B single
// producer; we MEASURE it and report the real figure (and the full curve), not
// an asserted number.
//
// ---------------------------------------------------------------------------
// What we measure and why this is an honest single-producer number
// ---------------------------------------------------------------------------
// Policy = Overwrite — the throughput-optimal mode (DESIGN §3): the producer
// NEVER blocks on a consumer, so the measured publish rate is the producer's
// own ceiling, not gated by how fast a consumer drains. That is exactly the
// "single producer throughput" the headline target names. We still spin up
// `consumers` real reader threads (pinned to their own cores) draining the ring
// concurrently, so the number includes the cache-coherence cost of fan-out
// (the producer's stores being pulled into N readers' caches) — i.e. it is the
// throughput *under* a given fan-out, not an unrealistic no-reader figure.
// consumers=0 is also reported as the pure publish ceiling.
//
// Timing: we time ONLY the producer's publish loop (steady_clock around N
// publishes), after a warmup batch and after pre-touching every ring page, so
// no first-touch page faults land in the timed window. throughput =
// N / elapsed. We repeat each cell `reps` times and keep the BEST (max
// throughput / min time) — the best run is the one least perturbed by OS jitter
// on a noisy machine, which is the right statistic for a "what can the hardware
// do" ceiling (min-time is standard for microbenchmarks; mean would fold in
// scheduler noise we are explicitly trying to exclude).
#include "common.hpp"

#include <shimmy/ring.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace shimmy::bench;

struct Cell {
  std::size_t block_size;
  int consumers;
  std::int64_t messages;
  std::int64_t best_elapsed_ns;
  double msgs_per_sec;
  double mib_per_sec;
};

// Keep the consumer's read from being optimized away without pulling in the
// whole <benchmark/benchmark.h> (the throughput harness is standalone, no
// Google Benchmark driver — it manages its own timing).
inline void benchmark_consume(const void* p) {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : "r"(p) : "memory");
#else
  (void)p;
#endif
}

// One throughput cell: BlockSize fixed at compile time, `consumers` reader
// threads draining concurrently, time the producer's N publishes. Capacity is
// generous (4096) so consumers have slack; Overwrite means the producer never
// waits regardless.
template <std::size_t BlockSize>
Cell run_cell(int consumers, std::int64_t messages, std::int64_t warmup,
              int reps, int producer_cpu, int first_consumer_cpu, int ncpu) {
  using Ring = shimmy::Ring<BlockSize, 4096, shimmy::Overwrite>;

  std::int64_t best_elapsed = INT64_MAX;

  for (int rep = 0; rep < reps; ++rep) {
    // Heap-allocate: a 4096-slot ring of 4KB blocks is ~17MB, which overflows
    // the default thread stack if placed as a local. The ring is non-copyable
    // and pointer-free, so a unique_ptr over the same storage is fine.
    auto ring_holder = std::make_unique<Ring>();
    Ring& ring = *ring_holder;
    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};
    std::atomic<int> ready{0};
    std::atomic<std::uint64_t> total_read{0};

    std::vector<std::thread> readers;
    readers.reserve(static_cast<std::size_t>(consumers));
    for (int ci = 0; ci < consumers; ++ci) {
      readers.emplace_back([&, ci] {
        int cpu = first_consumer_cpu + ci;
        if (cpu < ncpu) {
          pin_to_cpu(cpu);
        }
        shimmy::Consumer<Ring> c(ring);
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
          shimmy::Spin::pause();
        }
        std::uint64_t got = 0;
        // Drain as fast as possible; resync on lap (Overwrite). We don't
        // validate payload here — this thread exists to create realistic
        // coherence traffic, not to check correctness (that's the tests' job).
        while (!stop.load(std::memory_order_relaxed)) {
          auto view = c.read();
          const auto st = c.status();
          if (st == shimmy::read_status::ok) {
            benchmark_consume(view.data());
            c.commit();
            ++got;
          } else if (st == shimmy::read_status::lapped) {
            c.resync(ring.produced());
          } else { // empty
            shimmy::Spin::pause();
          }
        }
        total_read.fetch_add(got, std::memory_order_relaxed);
      });
    }

    pin_to_cpu(producer_cpu);
    // Wait for all readers to be registered & spinning.
    while (ready.load(std::memory_order_acquire) < consumers) {
      shimmy::Spin::pause();
    }

    alignas(64) std::uint8_t msg[BlockSize];
    std::memset(msg, 0x33, sizeof(msg));

    // Pre-touch every ring slot (publish a full Capacity worth) so the timed
    // loop faults nothing in. Also warms branch predictors / I-cache.
    for (std::int64_t i = 0; i < warmup; ++i) {
      ring.publish(msg, sizeof(msg));
    }

    start.store(true, std::memory_order_release);

    const std::int64_t t0 = now_ns();
    for (std::int64_t i = 0; i < messages; ++i) {
      ring.publish(msg, sizeof(msg));
    }
    const std::int64_t t1 = now_ns();

    stop.store(true, std::memory_order_release);
    for (auto& t : readers) {
      t.join();
    }

    const std::int64_t elapsed = t1 - t0;
    if (elapsed > 0 && elapsed < best_elapsed) {
      best_elapsed = elapsed;
    }
  }

  Cell cell;
  cell.block_size = BlockSize;
  cell.consumers = consumers;
  cell.messages = messages;
  cell.best_elapsed_ns = best_elapsed;
  const double secs = static_cast<double>(best_elapsed) / 1e9;
  cell.msgs_per_sec = secs > 0 ? static_cast<double>(messages) / secs : 0.0;
  const double bytes = static_cast<double>(messages) *
                       static_cast<double>(BlockSize);
  cell.mib_per_sec = secs > 0 ? (bytes / secs) / (1024.0 * 1024.0) : 0.0;
  return cell;
}

void emit(CsvWriter& csv, const Cell& c, const cpu_config& cfg) {
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "throughput,Overwrite,Spin,%zu,%d,%lld,%lld,%.1f,%.1f,%s,%s,%d",
                c.block_size, c.consumers,
                static_cast<long long>(c.messages),
                static_cast<long long>(c.best_elapsed_ns), c.msgs_per_sec,
                c.mib_per_sec, cfg.governor.c_str(), turbo_str(cfg).c_str(),
                cfg.noisy ? 1 : 0);
  csv.line(buf);
}

} // namespace

int main(int argc, char** argv) {
  const std::string out = arg_value(argc, argv, "--csv", "throughput.csv");
  const std::int64_t messages = arg_int(argc, argv, "--messages", 20000000);
  const std::int64_t warmup = arg_int(argc, argv, "--warmup", 100000);
  const int reps = static_cast<int>(arg_int(argc, argv, "--reps", 3));

  const cpu_config cfg = detect_cpu_config();
  print_cpu_config_banner(cfg);

  const int ncpu = num_online_cpus();
  const int producer_cpu = 0;
  // Spread consumers onto distinct cores starting at cpu 2 (leave cpu1 = SMT
  // sibling of producer alone for cleaner producer timing).
  const int first_consumer_cpu = (ncpu > 2) ? 2 : 1;
  std::fprintf(stderr,
               "# throughput: producer=cpu%d consumers from cpu%d (%d online), "
               "messages=%lld/cell warmup=%lld reps=%d, policy=Overwrite\n",
               producer_cpu, first_consumer_cpu, ncpu,
               static_cast<long long>(messages),
               static_cast<long long>(warmup), reps);

  CsvWriter csv(out);
  csv.line("kind,policy,wait_strategy,block_size,consumers,messages,"
           "elapsed_ns,throughput_msgs_per_sec,throughput_mib_per_sec,"
           "governor,turbo,noisy");

  // Consumer counts to sweep. 0 = pure publish ceiling; 1/2/4 = fan-out cost.
  const int consumer_counts[] = {0, 1, 2, 4};

  double headline_64_1c = 0.0; // @64B, 1 consumer == the headline cell

  for (int cc : consumer_counts) {
    // Skip consumer counts that exceed available cores (would oversubscribe and
    // measure scheduler thrash, not throughput).
    if (cc > 0 && first_consumer_cpu + cc > ncpu) {
      std::fprintf(stderr,
                   "# skipping consumers=%d (not enough cores: need %d, have "
                   "%d)\n",
                   cc, first_consumer_cpu + cc, ncpu);
      continue;
    }
    auto c64 = run_cell<64>(cc, messages, warmup, reps, producer_cpu,
                            first_consumer_cpu, ncpu);
    emit(csv, c64, cfg);
    if (cc == 1) {
      headline_64_1c = c64.msgs_per_sec;
    }
    auto c256 = run_cell<256>(cc, messages, warmup, reps, producer_cpu,
                              first_consumer_cpu, ncpu);
    emit(csv, c256, cfg);
    auto c1k = run_cell<1024>(cc, messages, warmup, reps, producer_cpu,
                              first_consumer_cpu, ncpu);
    emit(csv, c1k, cfg);
    auto c4k = run_cell<4096>(cc, messages, warmup, reps, producer_cpu,
                              first_consumer_cpu, ncpu);
    emit(csv, c4k, cfg);
  }

  // ---- Headline verdict vs DESIGN §5 ----
  const double target = 10e6;
  std::fprintf(stderr, "#\n# headline @64B (1 consumer): %.2f M msgs/sec  "
                       "(target >=10M) -> %s\n",
               headline_64_1c / 1e6,
               headline_64_1c >= target ? "MET" : "MISSED");
  if (cfg.noisy) {
    std::fprintf(stderr,
                 "# (CPU config is NOISY — number is real but indicative.)\n");
  }
  std::fprintf(stderr, "# throughput CSV written to %s\n", out.c_str());
  return 0;
}
