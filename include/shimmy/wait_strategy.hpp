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
  // `waiters` is the Ring's eventcount counter, used only by the blocking Futex
  // strategy; spinning strategies never park so they ignore it (the uniform
  // signature is what lets the consumer's wait loop be tag-dispatched).
  template <typename PublishedSeq>
  static void wait(const PublishedSeq& /*published*/,
                   std::uint64_t /*expected_at_least*/,
                   std::uint64_t /*iter*/,
                   std::atomic<std::int32_t>& /*waiters*/) noexcept {
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
                   std::uint64_t iter,
                   std::atomic<std::int32_t>& /*waiters*/) noexcept {
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
// already saw as too-small, so we cannot miss a wake: FUTEX_WAIT returns
// immediately (EAGAIN) if the word already moved between our check and the
// syscall.
//
// Conditional wake (eventcount): issuing a FUTEX_WAKE after EVERY publish is a
// wasted syscall (hundreds of ns) on the producer hot path whenever no consumer
// is actually parked — the common "consumer keeping up" case. We gate the wake
// on a producer-visible waiter counter (`waiters`, owned by the Ring): a
// consumer increments it before it commits to FUTEX_WAIT and decrements on wake;
// the producer only issues the syscall when the count is non-zero.
//
// No lost wakeup. The consumer's discipline is: (1) waiters++ (seq_cst), (2)
// RE-LOAD the slot stamp, (3) FUTEX_WAIT iff still behind. The producer's is:
// (1) publish the slot stamp (release), (2) LOAD waiters (seq_cst), wake iff >0.
// Suppose the producer skips the wake (saw waiters==0) yet a consumer sleeps.
// Then the producer's waiters load is before the consumer's waiters++ in the
// single seq_cst total order; therefore the consumer's step-2 re-load is also
// after the producer's publish in that order, so it observes the new stamp and
// does NOT sleep — contradiction. (And FUTEX_WAIT's own *word==cmp compare is a
// second backstop: the slot stamp IS the futex word, so a publish between the
// re-load and the syscall makes WAIT return EAGAIN.)
struct Futex {
  // Spin this many iterations before falling back to a blocking FUTEX_WAIT, to
  // absorb the common "producer is right behind us" case without a syscall.
  // (SpinThenYield exposes the analogous knob as its SpinLimit template param.)
  static constexpr std::uint64_t spin_before_syscall = 64;

  template <typename PublishedSeq>
  static void wait(const PublishedSeq& published,
                   std::uint64_t expected_at_least,
                   std::uint64_t iter,
                   std::atomic<std::int32_t>& waiters) noexcept {
    // Spin a little first to absorb the common "producer is right behind us"
    // case without paying a syscall.
    if (iter < spin_before_syscall) {
      Spin::pause();
      return;
    }
#if defined(__linux__)
    // Announce intent to sleep BEFORE the decisive stamp re-load, with seq_cst
    // so it sits in the same total order as the producer's waiters load (this is
    // the edge that closes the lost-wakeup window — see the struct comment).
    waiters.fetch_add(1, std::memory_order_seq_cst);

    // The futex word is the low 32 bits of the published sequence. We expect to
    // sleep while published < expected_at_least; pass the last-seen value as the
    // futex comparison value. seq_cst (not relaxed) so this re-load is ordered
    // after the increment in the total order; we re-validate with an acquire
    // load after waking regardless. This load only decides whether to sleep.
    const std::uint64_t seen = published.load(std::memory_order_seq_cst);
    if (seen >= expected_at_least) {
      waiters.fetch_sub(1, std::memory_order_seq_cst);
      return; // producer already caught up; don't sleep.
    }
    const std::uint32_t cmp = static_cast<std::uint32_t>(seen);
    const std::uint32_t* word = futex_word(published);
    // FUTEX_WAIT: sleep iff *word == cmp. If the producer published between our
    // load and here, *word != cmp and we return immediately (no lost wake).
    syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, cmp, nullptr, nullptr, 0);
    waiters.fetch_sub(1, std::memory_order_seq_cst);
#else
    (void)published;
    (void)expected_at_least;
    (void)waiters;
    std::this_thread::yield();
#endif
  }

  // Called by the producer after every publish. Skips the FUTEX_WAKE syscall
  // entirely when no consumer is parked (waiters == 0); otherwise wakes every
  // waiter on the just-published slot's stamp word.
  template <typename PublishedSeq>
  static void notify(PublishedSeq& published,
                     std::atomic<std::int32_t>& waiters) noexcept {
#if defined(__linux__)
    // seq_cst load pairs with the consumer's seq_cst increment (struct comment):
    // if we read 0 here, no consumer can be committing to a WAIT for a stamp we
    // already published. Common keep-up case: zero -> zero syscalls.
    if (waiters.load(std::memory_order_seq_cst) == 0) {
      return;
    }
    const std::uint32_t* word = futex_word(published);
    syscall(SYS_futex, word, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
#else
    (void)published;
    (void)waiters;
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
