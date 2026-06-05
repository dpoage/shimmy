// SPDX-License-Identifier: Apache-2.0
//
// shimmy LATENCY harness (bead shimmy-7d8, DESIGN §6).
//
// Measures END-TO-END producer-publish -> consumer-visible latency and records
// the full distribution (p50..p99.99) with HdrHistogram. Run ONCE PER WAIT
// STRATEGY (Spin, SpinThenYield, Futex) and report EACH tier's tail SEPARATELY
// (DESIGN §6 — we do NOT collapse to one universal ceiling). The design's
// per-tier p99.9 hypotheses (Spin<200ns, SpinThenYield<1us, Futex<10us) are
// printed alongside the MEASURED number as confirm/refute, but the deliverable
// is the measured tail — reported honestly even when it misses.
//
// ---------------------------------------------------------------------------
// How the cross-boundary timestamp works (without perturbing the measurement)
// ---------------------------------------------------------------------------
// The producer writes a send-timestamp (steady_clock ns) into the FIRST 8 bytes
// of each message payload, then publishes. The consumer zero-copy reads the
// slot, takes a recv-timestamp the instant it observes the message, reads the
// embedded send-timestamp back out, validate()s the read, and records
// (recv - send - clock_overhead) into the histogram. Both timestamps come from
// the SAME steady_clock on the SAME host (single NUMA node, DESIGN §8) so they
// are directly comparable; there is no clock-domain crossing. The send stamp
// lives in the payload we were going to memcpy anyway, so it adds no extra
// synchronization — the only added cost is the clock read, which we measure and
// subtract.
//
// ---------------------------------------------------------------------------
// Why we PACE the producer
// ---------------------------------------------------------------------------
// Latency is the time from "producer published" to "consumer saw it" for a
// consumer that is KEEPING UP. If the producer free-runs flat-out, the consumer
// falls behind and we'd measure queueing delay, not publish->visible latency
// (and under Overwrite we'd measure laps). So the producer sleeps a small,
// configurable inter-message gap (default ~2us) — long enough that the consumer
// is parked waiting on each message (which is exactly what exercises the wait
// strategy's wakeup cost, the whole point of the per-tier comparison).
//
// Methodology controls: pin producer/consumer to different cores; warm up
// (discard the first warmup_msgs samples) so caches/branch predictors/CPU
// frequency are settled before recording; measure & subtract clock overhead;
// pre-touch the ring so no page faults occur in the measured window.
#include "common.hpp"

#include <shimmy/ring.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace shimmy::bench;

// Fixed 64B block for the latency harness (the headline size). The first 8
// bytes carry the send timestamp; the rest is filler so the memcpy cost is
// representative of a real 64B message.
constexpr std::size_t kBlock = 64;

struct LatencyResult {
  std::int64_t p50, p90, p99, p999, p9999, lo, hi;
  double mean;
  std::int64_t samples;
};

// Run the latency harness for a given wait strategy. Returns the percentile
// summary (already overhead-corrected: we subtract clock overhead per sample).
template <typename Wait>
LatencyResult run_latency(std::int64_t measure_msgs, std::int64_t warmup_msgs,
                          std::int64_t gap_ns, double clock_overhead_ns,
                          int producer_cpu, int consumer_cpu) {
  // Backpressure so nothing is ever lost: the consumer must see every message
  // to record its latency, and the producer is paced anyway so it never stalls.
  using Ring = shimmy::Ring<kBlock, 1024, shimmy::Backpressure, 4, Wait>;
  auto ring_holder = std::make_unique<Ring>();
  Ring& ring = *ring_holder;

  const std::int64_t total = warmup_msgs + measure_msgs;
  const auto oh = static_cast<std::int64_t>(clock_overhead_ns + 0.5);

  Histogram hist(/*max_ns=*/10LL * 1000 * 1000 * 1000, /*sig=*/3);

  std::atomic<bool> consumer_ready{false};
  std::atomic<bool> done{false};
  Histogram* hp = &hist;

  std::thread consumer([&] {
    pin_to_cpu(consumer_cpu);
    shimmy::Consumer<Ring> c(ring);
    consumer_ready.store(true, std::memory_order_release);

    std::int64_t seen = 0;
    while (seen < total) {
      auto view = c.read_blocking();
      const std::int64_t recv = now_ns();
      if (c.status() != shimmy::read_status::ok) {
        // Backpressure never laps; read_blocking only returns non-ok on a
        // (here-impossible) lapped status. Defensive: skip.
        c.commit();
        continue;
      }
      std::int64_t send = 0;
      std::memcpy(&send, view.data(), sizeof(send));
      // validate() the zero-copy read before trusting the bytes (contract).
      if (!c.validate()) {
        // Under Backpressure this cannot happen (slot stable), but honor the
        // contract: re-read.
        continue;
      }
      c.commit();
      if (seen >= warmup_msgs) {
        std::int64_t lat = recv - send - oh;
        hp->record(lat);
      }
      ++seen;
    }
    done.store(true, std::memory_order_release);
  });

  // Producer on its own core.
  pin_to_cpu(producer_cpu);
  while (!consumer_ready.load(std::memory_order_acquire)) {
    shimmy::Spin::pause();
  }

  // Pre-touch: publish a few throwaway messages to fault-in the ring pages and
  // warm the path BEFORE the consumer's warmup window (the consumer counts from
  // seq 0, so these are part of `total`; we simply ensure the gap-pacing loop is
  // hot). The consumer's warmup_msgs discard handles statistical warmup.
  alignas(64) std::uint8_t msg[kBlock];
  std::memset(msg, 0x5A, sizeof(msg));

  std::int64_t next_send = now_ns();
  for (std::int64_t i = 0; i < total; ++i) {
    // Pace: busy-wait (not sleep) to the next send slot so we don't add the
    // scheduler's wakeup jitter to the producer side. gap_ns is small.
    if (gap_ns > 0) {
      while (now_ns() < next_send) {
        shimmy::Spin::pause();
      }
    }
    const std::int64_t send = now_ns();
    std::memcpy(msg, &send, sizeof(send));
    ring.publish(msg, sizeof(msg));
    next_send = send + gap_ns;
  }

  consumer.join();

  return LatencyResult{
      hist.value_at(50.0),  hist.value_at(90.0),   hist.value_at(99.0),
      hist.value_at(99.9),  hist.value_at(99.99),  hist.min(),
      hist.max(),           hist.mean(),           hist.count(),
  };
}

void emit_row(CsvWriter& csv, const std::string& strat, const LatencyResult& r,
              double clock_oh, const cpu_config& cfg) {
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "latency,%s,%zu,%lld,%.1f,%lld,%lld,%lld,%lld,%lld,%lld,%lld,"
                "%.1f,%s,%s,%d",
                strat.c_str(), kBlock, static_cast<long long>(r.samples),
                clock_oh, static_cast<long long>(r.p50),
                static_cast<long long>(r.p90), static_cast<long long>(r.p99),
                static_cast<long long>(r.p999),
                static_cast<long long>(r.p9999), static_cast<long long>(r.lo),
                static_cast<long long>(r.hi), r.mean, cfg.governor.c_str(),
                turbo_str(cfg).c_str(), cfg.noisy ? 1 : 0);
  csv.line(buf);
}

void print_verdict(const char* strat, std::int64_t p999, std::int64_t target,
                   const char* target_str) {
  const bool met = p999 <= target;
  std::fprintf(stderr,
               "#   %-14s p99.9=%6lld ns  (hypothesis %s)  -> %s\n", strat,
               static_cast<long long>(p999), target_str,
               met ? "MET" : "MISSED");
}

} // namespace

int main(int argc, char** argv) {
  const std::string out = arg_value(argc, argv, "--csv", "latency.csv");
  const std::int64_t measure_msgs =
      arg_int(argc, argv, "--samples", 200000);
  const std::int64_t warmup_msgs = arg_int(argc, argv, "--warmup", 20000);
  const std::int64_t gap_ns = arg_int(argc, argv, "--gap_ns", 2000);

  const cpu_config cfg = detect_cpu_config();
  print_cpu_config_banner(cfg);

  const double clock_oh = measure_clock_overhead_ns();
  std::fprintf(stderr, "# clock_overhead=%.1f ns (subtracted per sample)\n",
               clock_oh);

  // Pin producer/consumer to different physical cores. On SMT systems logical
  // CPUs 0 and 1 are usually siblings of core 0; use 0 and 2 to land on
  // distinct physical cores when possible.
  const int ncpu = num_online_cpus();
  const int producer_cpu = 0;
  const int consumer_cpu = (ncpu > 2) ? 2 : (ncpu > 1 ? 1 : 0);
  std::fprintf(stderr,
               "# pinning: producer=cpu%d consumer=cpu%d (%d online), "
               "samples=%lld warmup=%lld gap=%lldns, block=%zuB, "
               "policy=Backpressure\n",
               producer_cpu, consumer_cpu, ncpu,
               static_cast<long long>(measure_msgs),
               static_cast<long long>(warmup_msgs),
               static_cast<long long>(gap_ns), kBlock);

  CsvWriter csv(out);
  csv.line("kind,wait_strategy,block_size,samples,clock_overhead_ns,"
           "p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,min_ns,max_ns,mean_ns,"
           "governor,turbo,noisy");

  // ---- One run per wait strategy (DESIGN §6) ----
  auto spin = run_latency<shimmy::Spin>(measure_msgs, warmup_msgs, gap_ns,
                                        clock_oh, producer_cpu, consumer_cpu);
  emit_row(csv, "Spin", spin, clock_oh, cfg);

  auto sty = run_latency<shimmy::SpinThenYield<>>(
      measure_msgs, warmup_msgs, gap_ns, clock_oh, producer_cpu, consumer_cpu);
  emit_row(csv, "SpinThenYield", sty, clock_oh, cfg);

  auto futex = run_latency<shimmy::Futex>(measure_msgs, warmup_msgs, gap_ns,
                                          clock_oh, producer_cpu, consumer_cpu);
  emit_row(csv, "Futex", futex, clock_oh, cfg);

  // ---- Verdicts vs DESIGN §6 hypotheses ----
  std::fprintf(stderr, "#\n# per-strategy p99.9 vs DESIGN §6 hypotheses:\n");
  print_verdict("Spin", spin.p999, 200, "<200ns");
  print_verdict("SpinThenYield", sty.p999, 1000, "<1us");
  print_verdict("Futex", futex.p999, 10000, "<10us");
  if (cfg.noisy) {
    std::fprintf(stderr,
                 "# (CPU config is NOISY — treat verdicts as indicative.)\n");
  }
  std::fprintf(stderr, "# latency CSV written to %s\n", out.c_str());
  return 0;
}
