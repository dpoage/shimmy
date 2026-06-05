// SPDX-License-Identifier: Apache-2.0
//
// shimmy bench harness — shared measurement utilities (bead shimmy-7d8).
//
// This is the "data doesn't lie" backbone's plumbing: high-resolution timing,
// thread pinning, a CPU-noise detector that WARNS when the machine is in a
// config that produces untrustworthy tails (frequency scaling / turbo), and
// a tiny CSV emitter with a stable schema. The harness deliberately does NOT
// silently "correct" for a noisy machine — it measures honestly and tells you
// the config so a reader can judge the numbers (DESIGN guiding principle).
//
// Timing: we use std::chrono::steady_clock. On x86-64/Linux libstdc++ and
// libc++ both back steady_clock with clock_gettime(CLOCK_MONOTONIC), vDSO-
// accelerated (no syscall on the hot path). We measure and subtract the clock
// read overhead (see measure_clock_overhead_ns) so sub-100ns latencies are not
// swamped by the timer itself.
#ifndef SHIMMY_BENCH_COMMON_HPP
#define SHIMMY_BENCH_COMMON_HPP

#include <hdr/hdr_histogram.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

namespace shimmy::bench {

using clock_type = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

inline std::int64_t now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             clock_type::now().time_since_epoch())
      .count();
}

// Measure the irreducible cost of reading the clock, in nanoseconds. The
// latency harness subtracts this so the timer overhead is not reported as
// message latency. We take the MIN over many back-to-back reads (the cleanest
// estimate of the unavoidable read cost).
inline double measure_clock_overhead_ns() {
  constexpr int kIters = 200000;
  std::int64_t best = INT64_MAX;
  for (int i = 0; i < 10000; ++i) { // warm up
    volatile std::int64_t t = now_ns();
    (void)t;
  }
  for (int i = 0; i < kIters; ++i) {
    const std::int64_t a = now_ns();
    const std::int64_t b = now_ns();
    const std::int64_t d = b - a;
    if (d > 0 && d < best) {
      best = d;
    }
  }
  return (best == INT64_MAX) ? 0.0 : static_cast<double>(best);
}

// ---------------------------------------------------------------------------
// Thread pinning (single NUMA node assumption, DESIGN §8)
// ---------------------------------------------------------------------------

// Pin the CALLING thread to a single logical CPU. Returns true on success.
// The latency harness pins producer and consumer to DIFFERENT cores so the
// measured latency reflects real inter-core cache-coherence traffic — what a
// shmem fan-out actually pays — not the artificially low number two SMT
// siblings sharing L1/L2 would show.
inline bool pin_to_cpu(int cpu) {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
  (void)cpu;
  return false;
#endif
}

inline int num_online_cpus() {
#if defined(__linux__)
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? static_cast<int>(n) : 1;
#else
  return 1;
#endif
}

// ---------------------------------------------------------------------------
// CPU-noise detection — WARN, don't lie (the smoke bench already cautions about
// frequency scaling; here we actually detect it and surface it).
// ---------------------------------------------------------------------------

struct cpu_config {
  std::string governor;     // e.g. "performance", "powersave", "schedutil"
  bool turbo_known = false; // could we read the turbo/boost state?
  bool turbo_enabled = false;
  bool noisy = false;       // governor != performance, or boost on
  std::string note;         // human-readable summary of why it is/isn't noisy
};

inline std::string read_first_line(const char* path) {
  std::FILE* f = std::fopen(path, "r");
  if (!f) {
    return {};
  }
  char buf[256] = {0};
  if (!std::fgets(buf, sizeof(buf), f)) {
    std::fclose(f);
    return {};
  }
  std::fclose(f);
  std::string s(buf);
  while (!s.empty() &&
         (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
    s.pop_back();
  }
  return s;
}

inline cpu_config detect_cpu_config() {
  cpu_config c;
#if defined(__linux__)
  c.governor =
      read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
  // Turbo/boost: intel_pstate exposes no_turbo (1 == disabled); generic cpufreq
  // exposes cpufreq/boost (1 == enabled). Try both.
  const std::string no_turbo =
      read_first_line("/sys/devices/system/cpu/intel_pstate/no_turbo");
  const std::string boost =
      read_first_line("/sys/devices/system/cpu/cpufreq/boost");
  if (!no_turbo.empty()) {
    c.turbo_known = true;
    c.turbo_enabled = (no_turbo == "0");
  } else if (!boost.empty()) {
    c.turbo_known = true;
    c.turbo_enabled = (boost == "1");
  }

  const bool gov_perf = (c.governor == "performance");
  c.noisy = (!c.governor.empty() && !gov_perf) || c.turbo_enabled;

  if (c.governor.empty()) {
    c.note = "governor unknown (cpufreq sysfs not readable)";
  } else if (gov_perf && c.turbo_known && !c.turbo_enabled) {
    c.note = "governor=performance, turbo off — clean config for stable tails";
  } else {
    c.note = "governor=" + c.governor;
    if (c.turbo_known) {
      c.note += c.turbo_enabled ? ", turbo ON" : ", turbo off";
    } else {
      c.note += ", turbo state unknown";
    }
    if (c.noisy) {
      c.note +=
          " -> NOISY: frequency may scale during the run; tails are indicative,"
          " not authoritative. For stable numbers: set governor=performance and"
          " disable turbo, pin to isolated cores.";
    }
  }
#else
  c.note = "non-Linux: CPU config detection unavailable";
#endif
  return c;
}

inline void print_cpu_config_banner(const cpu_config& c) {
  std::fprintf(stderr, "# shimmy-bench cpu-config: %s\n", c.note.c_str());
  if (c.noisy) {
    std::fprintf(stderr,
                 "# WARNING: measuring under a NOISY CPU config. Numbers are "
                 "real but not authoritative — see note above.\n");
  }
}

// ---------------------------------------------------------------------------
// HdrHistogram RAII wrapper
// ---------------------------------------------------------------------------

class Histogram {
public:
  // 1ns .. max_ns, `sig` significant figures. Default range 1ns..10s covers
  // every wait strategy's tail (Futex blocks can be ms-scale under load).
  explicit Histogram(std::int64_t max_ns = 10LL * 1000 * 1000 * 1000,
                     int sig = 3) {
    if (hdr_init(1, max_ns, sig, &h_) != 0) {
      h_ = nullptr;
    }
  }
  ~Histogram() {
    if (h_) {
      hdr_close(h_);
    }
  }
  Histogram(const Histogram&) = delete;
  Histogram& operator=(const Histogram&) = delete;

  bool ok() const noexcept { return h_ != nullptr; }
  void record(std::int64_t value_ns) noexcept {
    if (!h_) {
      return;
    }
    // Clamp sub-ns / negative-after-overhead-subtraction values to the 1ns
    // floor of the histogram rather than dropping them.
    hdr_record_value(h_, value_ns >= 1 ? value_ns : 1);
  }
  std::int64_t value_at(double pct) const noexcept {
    return h_ ? hdr_value_at_percentile(h_, pct) : -1;
  }
  std::int64_t min() const noexcept { return h_ ? hdr_min(h_) : -1; }
  std::int64_t max() const noexcept { return h_ ? hdr_max(h_) : -1; }
  double mean() const noexcept { return h_ ? hdr_mean(h_) : -1.0; }
  std::int64_t count() const noexcept { return h_ ? h_->total_count : 0; }

private:
  hdr_histogram* h_ = nullptr;
};

// ---------------------------------------------------------------------------
// CSV emitter — stable schema (documented at the top of latency_bench.cpp /
// throughput_bench.cpp). Each row is echoed to stdout AND written to file so a
// terminal/CI run shows the table inline.
// ---------------------------------------------------------------------------

inline std::string turbo_str(const cpu_config& c) {
  if (!c.turbo_known) {
    return "unknown";
  }
  return c.turbo_enabled ? "on" : "off";
}

class CsvWriter {
public:
  explicit CsvWriter(const std::string& path) {
    if (!path.empty()) {
      f_ = std::fopen(path.c_str(), "w");
    }
  }
  ~CsvWriter() {
    if (f_) {
      std::fclose(f_);
    }
  }
  CsvWriter(const CsvWriter&) = delete;
  CsvWriter& operator=(const CsvWriter&) = delete;

  void line(const std::string& s) {
    if (f_) {
      std::fprintf(f_, "%s\n", s.c_str());
    }
    std::printf("%s\n", s.c_str());
  }

private:
  std::FILE* f_ = nullptr;
};

// Parse a simple --flag=value CLI arg. Returns the value or `def`.
inline std::string arg_value(int argc, char** argv, const std::string& flag,
                             const std::string& def) {
  const std::string prefix = flag + "=";
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a.rfind(prefix, 0) == 0) {
      return a.substr(prefix.size());
    }
  }
  return def;
}

inline std::int64_t arg_int(int argc, char** argv, const std::string& flag,
                            std::int64_t def) {
  const std::string v = arg_value(argc, argv, flag, "");
  if (v.empty()) {
    return def;
  }
  return std::strtoll(v.c_str(), nullptr, 10);
}

} // namespace shimmy::bench

#endif // SHIMMY_BENCH_COMMON_HPP
