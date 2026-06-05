// SPDX-License-Identifier: Apache-2.0
//
// shimmy — consumer wait strategies (tag types, DESIGN §6).
//
// A wait strategy decides HOW a consumer waits for the producer to publish the
// sequence it is looking for. It is consumer-owned and compile-time selected:
// the strategy is a tag type whose `wait()` member is chosen via tag dispatch,
// so the consumer's wait loop is monomorphized with no virtual calls.
//
// Latency-vs-CPU tiers (each benched separately in shimmy-7d8):
//   * Spin          — busy-poll, tightest tail, burns a core.
//   * SpinThenYield  — spin briefly then sched_yield, robust default.
//   * Futex          — block in the kernel on the published sequence word.
//
// IMPORTANT: a wait strategy MUST NOT change the memory-ordering contract. The
// strategy only governs *when* we re-poll the published sequence; the acquire
// load of that sequence (in ring.hpp) is what establishes visibility. A futex
// wake is not itself a synchronization edge for the payload — after a wake the
// consumer still performs the acquire load and re-checks, so correctness never
// depends on the futex's own ordering.
#ifndef SHIMMY_WAIT_STRATEGY_HPP
#define SHIMMY_WAIT_STRATEGY_HPP

#include <atomic>
#include <cstdint>

#if defined(__linux__)
#include <climits>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <thread>

namespace shimmy {

// Spin: pure busy-wait. We still emit a pause hint to be polite to the core's
// pipeline / a sibling hyperthread; on non-x86 it degrades to a relaxed nop.
struct Spin {
  static void pause() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
  }

  // `published` is the producer's published-sequence atomic. We never use its
  // value here for ordering — the caller re-does the real acquire load. `iter`
  // is the spin iteration count so SpinThenYield can share this signature.
  template <typename PublishedSeq>
  static void wait(const PublishedSeq& /*published*/,
                   std::uint64_t /*expected_at_least*/,
                   std::uint64_t /*iter*/) noexcept {
    pause();
  }

  static constexpr bool blocks = false;
};

// SpinThenYield: spin for a bounded number of iterations, then yield the CPU.
// `SpinLimit` controls when we transition. Strong general-purpose default:
// keeps the tail low when the producer is hot, but does not monopolize a core
// under oversubscription.
template <std::uint64_t SpinLimit = 1024>
struct SpinThenYield {
  template <typename PublishedSeq>
  static void wait(const PublishedSeq& /*published*/,
                   std::uint64_t /*expected_at_least*/,
                   std::uint64_t iter) noexcept {
    if (iter < SpinLimit) {
      Spin::pause();
    } else {
      std::this_thread::yield();
    }
  }

  static constexpr bool blocks = false;
};

// Futex: block in the kernel until the producer's published-sequence word
// changes. CPU-friendly under oversubscription. The futex operates on the
// 32-bit low half of the published sequence (futex words are 32-bit on Linux).
// We block only while the *observed* published value still equals the value we
// already saw as too-small, so we cannot miss a wake: the producer issues a
// FUTEX_WAKE after every publish (see ring.hpp), and FUTEX_WAIT returns
// immediately (EAGAIN) if the word already moved between our check and the
// syscall.
struct Futex {
  template <typename PublishedSeq>
  static void wait(const PublishedSeq& published,
                   std::uint64_t expected_at_least,
                   std::uint64_t iter) noexcept {
    // Spin a little first to absorb the common "producer is right behind us"
    // case without paying a syscall.
    if (iter < 64) {
      Spin::pause();
      return;
    }
#if defined(__linux__)
    // The futex word is the low 32 bits of the published sequence. We expect to
    // sleep while published < expected_at_least; pass the last-seen value as the
    // futex comparison value. A relaxed load is fine: the kernel re-reads the
    // word atomically under the futex lock, and we re-validate with an acquire
    // load after waking. This load only decides whether to attempt to sleep.
    const std::uint64_t seen =
        published.load(std::memory_order_relaxed);
    if (seen >= expected_at_least) {
      return; // producer already caught up; don't sleep.
    }
    const std::uint32_t cmp = static_cast<std::uint32_t>(seen);
    const std::uint32_t* word = futex_word(published);
    // FUTEX_WAIT: sleep iff *word == cmp. If the producer published between our
    // load and here, *word != cmp and we return immediately (no lost wake).
    syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, cmp, nullptr, nullptr, 0);
#else
    std::this_thread::yield();
#endif
  }

  // Called by the producer after every publish to wake any blocked consumers.
  template <typename PublishedSeq>
  static void notify(PublishedSeq& published) noexcept {
#if defined(__linux__)
    const std::uint32_t* word = futex_word(published);
    syscall(SYS_futex, word, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
#else
    (void)published;
#endif
  }

  static constexpr bool blocks = true;

private:
  // The futex address must be the low 32 bits of the 64-bit published sequence.
  // std::atomic<std::uint64_t> is required to be address-free and lock-free on
  // our targets, and on little-endian x86-64 its first 4 bytes ARE the low word.
  // We assert little-endianness so this aliasing is valid; phase 1 is x86-64
  // only (DESIGN §8), so this is sound.
  template <typename PublishedSeq>
  static const std::uint32_t* futex_word(const PublishedSeq& published) noexcept {
    static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
                  "Futex wait strategy assumes little-endian (x86-64 phase 1)");
    return reinterpret_cast<const std::uint32_t*>(&published);
  }
};

template <typename T>
struct is_wait_strategy : std::false_type {};
template <>
struct is_wait_strategy<Spin> : std::true_type {};
template <std::uint64_t N>
struct is_wait_strategy<SpinThenYield<N>> : std::true_type {};
template <>
struct is_wait_strategy<Futex> : std::true_type {};
template <typename T>
inline constexpr bool is_wait_strategy_v = is_wait_strategy<T>::value;

} // namespace shimmy

#endif // SHIMMY_WAIT_STRATEGY_HPP
