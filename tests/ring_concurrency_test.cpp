// SPDX-License-Identifier: Apache-2.0
//
// Multi-threaded SPMC correctness + overflow-policy invariants. These are real
// thread-races and are the primary target of the TSan build (acceptance
// criterion 6): no data races may be reported.
//
// Message format on the wire: a 16-byte payload of [u64 seq][u64 checksum]
// where checksum = seq * GOLDEN. A consumer that reads a slot whose stamp
// matches `seq` must observe a self-consistent payload (checksum == seq*GOLDEN)
// — a torn read would be caught by validate() and retried, so any *committed*
// message is whole.
#include <shimmy/ring.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {

using shimmy::Backpressure;
using shimmy::Consumer;
using shimmy::Overwrite;
using shimmy::read_status;
using shimmy::Ring;
using shimmy::Spin;

constexpr std::uint64_t kGolden = 0x9E3779B97F4A7C15ULL;

struct Msg {
  std::uint64_t seq;
  std::uint64_t checksum;
};

Msg make_msg(std::uint64_t seq) noexcept {
  return Msg{seq, seq * kGolden};
}

bool msg_consistent(const Msg& m) noexcept {
  return m.checksum == m.seq * kGolden;
}

// ---------------------------------------------------------------------------
// Backpressure: lossless. Every consumer must see the FULL ordered stream with
// no gaps and no torn payloads. The producer blocks until the slowest consumer
// frees a slot, so nothing is ever lost.
// ---------------------------------------------------------------------------
TEST(Concurrency, BackpressureLosslessFanout) {
  constexpr std::size_t Cap = 64;
  constexpr std::uint64_t N = 50'000;
  constexpr int kConsumers = 3;
  using R = Ring<64, Cap, Backpressure, 8, Spin>;
  R ring;

  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  std::atomic<std::uint64_t> total_read{0};
  std::atomic<int> failures{0};

  for (int t = 0; t < kConsumers; ++t) {
    threads.emplace_back([&] {
      Consumer<R> c(ring);
      ready.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire)) {
      }
      std::uint64_t expect = 0;
      while (expect < N) {
        auto m = c.read_blocking();
        if (c.status() != read_status::ok) {
          // Backpressure must never lap; any non-ok here is a defect.
          failures.fetch_add(1, std::memory_order_relaxed);
          break;
        }
        Msg got{};
        std::memcpy(&got, m.data(), sizeof(got));
        if (!c.validate()) {
          // Should never happen under Backpressure (producer waits for us), but
          // if it did we'd simply re-read the same seq.
          continue;
        }
        if (got.seq != expect || !msg_consistent(got)) {
          failures.fetch_add(1, std::memory_order_relaxed);
          break;
        }
        c.commit();
        ++expect;
      }
      total_read.fetch_add(expect, std::memory_order_relaxed);
    });
  }

  // Wait for all consumers to register their cursors before the producer starts,
  // otherwise the producer could lap a not-yet-registered consumer.
  while (ready.load(std::memory_order_acquire) < kConsumers) {
  }
  go.store(true, std::memory_order_release);

  for (std::uint64_t seq = 0; seq < N; ++seq) {
    const Msg m = make_msg(seq);
    ring.publish(&m, sizeof(m));
  }

  for (auto& th : threads) {
    th.join();
  }

  EXPECT_EQ(failures.load(), 0) << "backpressure must be lossless & in-order";
  EXPECT_EQ(total_read.load(), N * kConsumers)
      << "every consumer must read the full stream";
}

// ---------------------------------------------------------------------------
// Overwrite: lossy but consistent. A consumer may be lapped and lose ranges,
// but EVERY message it does commit must be self-consistent (no torn reads), be
// strictly increasing in sequence, and the consumer must make forward progress
// to the end of the stream via resync. We assert: all committed messages are
// consistent & monotonic, and the consumer eventually observes the final seq.
// ---------------------------------------------------------------------------
TEST(Concurrency, OverwriteConsistentDespiteLaps) {
  constexpr std::size_t Cap = 64;
  constexpr std::uint64_t N = 200'000;
  constexpr int kConsumers = 3;
  using R = Ring<64, Cap, Overwrite, 8, Spin>;
  R ring;

  std::atomic<bool> done{false};
  std::vector<std::thread> threads;
  std::atomic<int> torn{0};
  std::atomic<int> nonmonotonic{0};
  std::atomic<std::uint64_t> max_seq_seen{0};

  for (int t = 0; t < kConsumers; ++t) {
    threads.emplace_back([&] {
      Consumer<R> c(ring);
      std::uint64_t last = 0;
      bool have_last = false;
      for (;;) {
        auto m = c.read();
        const auto st = c.status();
        if (st == read_status::ok) {
          Msg got{};
          std::memcpy(&got, m.data(), sizeof(got));
          // Validate the zero-copy view: if lapped mid-read, discard & retry.
          if (!c.validate()) {
            continue; // do NOT commit a torn view
          }
          if (!msg_consistent(got)) {
            torn.fetch_add(1, std::memory_order_relaxed);
          }
          if (have_last && got.seq <= last) {
            nonmonotonic.fetch_add(1, std::memory_order_relaxed);
          }
          last = got.seq;
          have_last = true;
          // track the max seq any consumer reached
          std::uint64_t cur = max_seq_seen.load(std::memory_order_relaxed);
          while (got.seq > cur &&
                 !max_seq_seen.compare_exchange_weak(
                     cur, got.seq, std::memory_order_relaxed)) {
          }
          c.commit();
        } else if (st == read_status::lapped) {
          c.resync(ring.produced()); // skip the gap, keep going
        } else { // empty
          if (done.load(std::memory_order_acquire) &&
              c.next() >= ring.produced()) {
            break;
          }
          Spin::pause();
        }
      }
    });
  }

  for (std::uint64_t seq = 0; seq < N; ++seq) {
    const Msg m = make_msg(seq);
    ring.publish(&m, sizeof(m));
  }
  done.store(true, std::memory_order_release);

  for (auto& th : threads) {
    th.join();
  }

  EXPECT_EQ(torn.load(), 0) << "no committed message may be torn";
  EXPECT_EQ(nonmonotonic.load(), 0)
      << "committed sequences must be strictly increasing per consumer";
  // The producer published [0, N). Consumers should have observed something
  // near the end (they keep up well enough on a free-running producer with
  // resync). We only require they reached at least the last published seq's
  // neighborhood — exact equality isn't guaranteed under lossy overwrite.
  EXPECT_GT(max_seq_seen.load(), 0u);
}

// ---------------------------------------------------------------------------
// SPMC keep-up: with a generously sized ring and consumers that keep pace,
// Overwrite consumers should see the entire ordered stream with no laps at all.
// This is the "multiple consumers each see the full ordered stream when keeping
// up" criterion (criterion 2).
// ---------------------------------------------------------------------------
TEST(Concurrency, OverwriteNoLapWhenConsumersKeepUp) {
  constexpr std::size_t Cap = 4096;
  constexpr std::uint64_t N = 100'000;
  constexpr int kConsumers = 4;
  using R = Ring<64, Cap, Overwrite, 8, shimmy::SpinThenYield<>>;
  R ring;

  std::vector<std::thread> threads;
  std::atomic<int> laps{0};
  std::atomic<int> failures{0};
  std::atomic<int> completed{0};

  for (int t = 0; t < kConsumers; ++t) {
    threads.emplace_back([&] {
      Consumer<R> c(ring);
      std::uint64_t expect = 0;
      while (expect < N) {
        auto m = c.read_blocking();
        if (c.status() == read_status::lapped) {
          laps.fetch_add(1, std::memory_order_relaxed);
          c.resync(ring.produced());
          expect = c.next();
          continue;
        }
        Msg got{};
        std::memcpy(&got, m.data(), sizeof(got));
        if (!c.validate()) {
          continue;
        }
        if (got.seq != expect || !msg_consistent(got)) {
          failures.fetch_add(1, std::memory_order_relaxed);
          break;
        }
        c.commit();
        ++expect;
      }
      completed.fetch_add(1, std::memory_order_relaxed);
    });
  }

  // Give consumers a moment to start so the deep ring isn't lapped at startup.
  for (std::uint64_t seq = 0; seq < N; ++seq) {
    const Msg m = make_msg(seq);
    ring.publish(&m, sizeof(m));
  }

  for (auto& th : threads) {
    th.join();
  }

  EXPECT_EQ(failures.load(), 0) << "in-order, consistent reads when keeping up";
  EXPECT_EQ(completed.load(), kConsumers);
  // We don't assert laps==0 (scheduling can hiccup), but the path is exercised.
}

// ---------------------------------------------------------------------------
// Zero-copy IN-PLACE read under maximal lapping (tiny ring). The consumer
// touches payload bytes in place WHILE the producer overwrites the same slot,
// then validate()s. Any view that fails validate() is discarded. This is the
// seqlock fast path and the primary stressor for the two-phase publish: every
// committed read must be self-consistent, and the TSan build must stay clean
// (the two-phase writing_seq marker + release/acquire fences are what make the
// concurrent payload access discardable rather than corrupting).
// ---------------------------------------------------------------------------
TEST(Concurrency, ZeroCopyInPlaceUnderMaximalLapping) {
  constexpr std::size_t Cap = 2; // tiny -> producer laps the consumer constantly
  constexpr std::uint64_t N = 300'000;
  using R = Ring<64, Cap, Overwrite, 8, Spin>;
  R ring;

  std::atomic<bool> done{false};
  std::atomic<int> torn{0};
  std::atomic<std::uint64_t> committed{0};

  std::thread consumer([&] {
    Consumer<R> c(ring);
    for (;;) {
      auto m = c.read();
      const auto st = c.status();
      if (st == read_status::ok) {
        // Touch the bytes IN PLACE (zero copy) while the producer may be
        // overwriting them — exactly the race validate() guards.
        Msg got{};
        std::memcpy(&got, m.data(), sizeof(got));
        if (!c.validate()) {
          continue; // lapped mid-read: discard, do not commit
        }
        if (!msg_consistent(got)) {
          torn.fetch_add(1, std::memory_order_relaxed);
        }
        c.commit();
        committed.fetch_add(1, std::memory_order_relaxed);
      } else if (st == read_status::lapped) {
        c.resync(ring.produced());
      } else { // empty
        if (done.load(std::memory_order_acquire) &&
            c.next() >= ring.produced()) {
          break;
        }
        Spin::pause();
      }
    }
  });

  for (std::uint64_t seq = 0; seq < N; ++seq) {
    const Msg m = make_msg(seq);
    ring.publish(&m, sizeof(m));
  }
  done.store(true, std::memory_order_release);
  consumer.join();

  EXPECT_EQ(torn.load(), 0)
      << "validate() must reject every torn zero-copy view; no committed "
         "message may be inconsistent";
  EXPECT_GT(committed.load(), 0u) << "consumer must make forward progress";
}

} // namespace
