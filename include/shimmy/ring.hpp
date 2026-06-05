// SPDX-License-Identifier: Apache-2.0
//
// shimmy — core single-producer / multi-consumer fixed-block ring buffer.
//
// This is the heart of shimmy (bead shimmy-4d4). It is validated IN-PROCESS
// here; the same layout is designed to later live in a shared-memory segment
// (bead shimmy-52e) WITHOUT MODIFICATION. To make that possible the structure
// obeys these rules, which shimmy-52e MUST preserve:
//
//   * NO internal pointers. Storage is an inline std::array; consumers address
//     slots by index (seq & mask), never by stored pointer. A Consumer holds an
//     integer cursor-slot id, not a back-pointer.
//   * Standard-layout, no virtuals, no vtables. The Ring can be
//     placement-new'd over an mmap'd region by a future bead.
//   * All cross-thread state is std::atomic over fixed-width integers, so the
//     bytes are position-independent and identical across processes mapping the
//     segment at different virtual addresses.
//
// ----------------------------------------------------------------------------
// Publish / consume protocol (DESIGN §2)
// ----------------------------------------------------------------------------
// Each slot carries its own atomic sequence stamp `seq`. The producer writes
// len + payload into slot (seq & mask) with NON-atomic stores, then publishes
// by storing `slot.seq = seq` with memory_order_release. The consumer loads
// `slot.seq` with memory_order_acquire; if it observes the sequence it expects,
// the release/acquire pair guarantees the len + payload stores are visible.
// The published-sequence stamp is the single synchronization point.
//
// Why release on publish / acquire on read, and why that is both sufficient and
// necessary:
//   * The producer's payload writes are plain stores. Release on the seq store
//     prevents them from being reordered AFTER the publish (compiler or CPU), so
//     a consumer that sees the new seq is guaranteed to see the payload. RELEASE
//     is the minimum that gives this; relaxed would allow the payload writes to
//     be observed after the stamp (broken), seq_cst is stronger than needed (no
//     cross-variable total order is required here).
//   * The consumer's acquire load on seq pairs with that release: it prevents
//     the subsequent payload reads from being hoisted BEFORE the stamp check.
//     ACQUIRE is the minimum; relaxed would allow reading stale payload bytes,
//     seq_cst is unnecessary.
// On x86-64 (TSO) both compile to plain MOVs (no fences) — we get correctness on
// weaker models for free, and pay nothing on the target. (DESIGN §8.)
//
// ----------------------------------------------------------------------------
// The zero-copy payload race under Overwrite (read THIS before touching it)
// ----------------------------------------------------------------------------
// Under the Overwrite policy a consumer holding a zero-copy span CAN have its
// payload bytes overwritten by the producer concurrently. That payload access
// is the classic *seqlock benign race*: the producer's payload stores and the
// consumer's payload loads are non-atomic and may overlap. Correctness does NOT
// come from those accesses being atomic — it comes from the two-phase publish
// (writing_seq marker, below) + validate(): the consumer reads, then validate()
// re-checks the stamp; if the producer touched the slot the stamp is no longer
// the wanted seq, so the (possibly torn) bytes are DISCARDED, never committed.
// This is intentional and matches DESIGN §2. The copying path read_copy() has
// the same race window but also validates and reports 0 on a torn copy.
// Under Backpressure the producer never overwrites an unread slot, so the
// zero-copy view is fully stable and there is no race at all.
// TSan note: this seqlock-shaped access is verified clean under TSan across
// millions of forced laps (tiny-ring stress test), but it remains a benign race
// in the strict C++ model — do not "fix" it by making payload atomic without
// measuring; validate() is the contract.
#ifndef SHIMMY_RING_HPP
#define SHIMMY_RING_HPP

#include <shimmy/detail/cache.hpp>
#include <shimmy/policies.hpp>
#include <shimmy/wait_strategy.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

namespace shimmy {

// Sentinel stamp meaning "this slot has never been published". Real sequence
// numbers start at 0, so we use ~0. A slot's stamp is monotonically increasing
// once publishing begins.
inline constexpr std::uint64_t empty_seq = ~std::uint64_t{0};

// Sentinel stamp meaning "the producer is CURRENTLY overwriting this slot".
//
// This is the seqlock "writing" marker and it is what makes the zero-copy read
// correct under Overwrite. A SINGLE trailing release stamp is NOT sufficient:
// when the producer overwrites slot i (occupant seq K) with seq K+Capacity, it
// writes the payload bytes while the stamp still reads K. A consumer reading
// seq K could then: (1) see stamp==K, (2) memcpy a payload being torn by the
// concurrent overwrite, (3) re-check stamp==K (producer hasn't done its final
// release yet) and wrongly conclude the read was clean. The window where
// validate() observes the OLD stamp on BOTH sides of a concurrent payload write
// is real (TSan-confirmed) and produces torn, non-monotonic reads.
//
// Fix (two-phase publish): before touching the payload the producer stores this
// `writing_seq` marker with release, then writes payload, then stores the final
// seq with release. A consumer's read is valid iff the stamp it saw BEFORE the
// payload read equals the wanted seq AND validate() sees that SAME seq after —
// any concurrent overwrite is now bracketed by writing_seq, which validate()
// will observe and reject. (Real sequence numbers can never equal this marker.)
inline constexpr std::uint64_t writing_seq = ~std::uint64_t{0} - 1;

// Result of a consumer read attempt.
enum class read_status : std::uint8_t {
  ok,       // a fresh message was returned
  empty,    // nothing new published yet (producer hasn't reached us)
  lapped,   // Overwrite only: producer lapped us; messages were lost
};

// ----------------------------------------------------------------------------
// Slot: one fixed-size block plus its publish stamp.
// ----------------------------------------------------------------------------
// Layout intent (DESIGN §2): [u32 len][payload bytes...][unused tail], with the
// atomic publish stamp co-located. The whole slot is cache-line aligned so a
// producer publishing slot i never false-shares the stamp of slot j.
template <std::size_t BlockSize>
struct alignas(detail::cache_line_size) Slot {
  // Publish stamp. RELEASE-stored by producer, ACQUIRE-loaded by consumers.
  std::atomic<std::uint64_t> seq;
  // Real payload length for the message currently in this slot.
  std::uint32_t len;
  // Payload region. BlockSize is the *block* (slot data) budget; the stamp and
  // len are bookkeeping that live alongside it.
  std::array<std::byte, BlockSize> payload;

  static constexpr std::size_t block_size = BlockSize;
  static constexpr std::size_t max_payload = BlockSize;
};

// ----------------------------------------------------------------------------
// Ring
// ----------------------------------------------------------------------------
//   BlockSize : bytes of usable payload per slot (NTTP).
//   Capacity  : number of slots, MUST be a power of two (index via mask).
//   Overflow  : Overwrite | Backpressure tag (DESIGN §3).
//   MaxConsumers : number of inline consumer cursor slots (for Backpressure).
//   Wait      : consumer wait strategy tag (DESIGN §6).
template <std::size_t BlockSize,
          std::size_t Capacity,
          typename Overflow = Overwrite,
          std::size_t MaxConsumers = 16,
          typename Wait = SpinThenYield<>>
class Ring {
  static_assert(is_overflow_policy_v<Overflow>,
                "Overflow must be shimmy::Overwrite or shimmy::Backpressure");
  static_assert(is_wait_strategy_v<Wait>,
                "Wait must be a shimmy wait strategy tag");
  static_assert(Capacity >= 2, "Capacity must be at least 2");
  static_assert((Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of two (index is seq & (Capacity-1))");
  static_assert(BlockSize % detail::cache_line_size == 0,
                "BlockSize should be a multiple of the cache line for clean "
                "slot alignment / no false sharing across slots");
  static_assert(MaxConsumers >= 1, "MaxConsumers must be >= 1");

public:
  using slot_type = Slot<BlockSize>;
  static constexpr std::size_t capacity = Capacity;
  static constexpr std::size_t block_size = BlockSize;
  static constexpr std::size_t max_consumers = MaxConsumers;
  static constexpr std::size_t mask = Capacity - 1;
  using overflow_policy = Overflow;
  using wait_strategy = Wait;

  Ring() noexcept { init(); }

  // Re-initialize an existing region (used by shimmy-52e when placement-newing
  // over fresh shared memory). Resets all stamps to empty.
  void init() noexcept {
    next_publish_.store(0, std::memory_order_relaxed);
    for (auto& s : slots_) {
      s.seq.store(empty_seq, std::memory_order_relaxed);
      s.len = 0;
    }
    for (auto& c : cursors_) {
      c.value.store(0, std::memory_order_relaxed);
      c.in_use.store(false, std::memory_order_relaxed);
    }
    // Make the freshly-initialized state visible before any thread observes the
    // ring (publisher/consumer construction happens-before use via the release
    // here paired with their first acquire).
    std::atomic_thread_fence(std::memory_order_release);
  }

  // Not copyable/movable: the ring owns its storage in place and may be shared
  // by address (and later mapped). Rule of five -> deleted.
  Ring(const Ring&) = delete;
  Ring& operator=(const Ring&) = delete;
  Ring(Ring&&) = delete;
  Ring& operator=(Ring&&) = delete;
  ~Ring() = default;

  // ------------------------------------------------------------------------
  // Producer side
  // ------------------------------------------------------------------------

  // Publish one message. Returns the sequence number assigned. `len` must be
  // <= BlockSize. Single-producer: NOT safe to call from multiple threads.
  std::uint64_t publish(const void* data, std::uint32_t len) noexcept {
    const std::uint64_t seq = next_publish_.load(std::memory_order_relaxed);
    slot_type& slot = slots_[seq & mask];

    // Backpressure: before we may reuse this slot we must wait until every
    // active consumer has consumed the message that currently occupies it
    // (i.e. the message with sequence seq - Capacity). For Overwrite this whole
    // block is `if constexpr`-eliminated — the producer free-runs.
    if constexpr (std::is_same_v<Overflow, Backpressure>) {
      if (seq >= Capacity) {
        const std::uint64_t must_be_consumed = seq - Capacity;
        await_consumers_past(must_be_consumed);
      }
    }

    // PHASE 1: mark the slot "being written" BEFORE touching payload. Release so
    // that a consumer which later observes writing_seq (or the final seq) has a
    // synchronization edge; more importantly this marker invalidates any
    // in-flight zero-copy reader of the previous occupant — its validate() will
    // see writing_seq (or the new seq), never a stale-but-equal old stamp.
    // For the very first write to a slot (occupant == empty_seq) this is
    // strictly unnecessary but harmless and keeps the path branch-free.
    slot.seq.store(writing_seq, std::memory_order_release);

    // Ensure the writing marker is globally ordered before the payload stores.
    // On x86-64 the prior release store + the following plain stores already
    // can't be reordered in a way that matters (TSO), but on weaker models we
    // need the marker visible before payload mutation; a release fence pins it.
    std::atomic_thread_fence(std::memory_order_release);

    // Write payload + length with PLAIN (non-atomic) stores, bracketed by the
    // writing marker (above) and the final publish (below).
    slot.len = len;
    if (len > 0) {
      std::memcpy(slot.payload.data(), data, len);
    }

    // PHASE 2 / PUBLISH: release-store the final stamp. Pairs with the
    // consumer's acquire load. This is the edge that makes len+payload visible
    // and signals "slot now holds seq".
    slot.seq.store(seq, std::memory_order_release);

    // Advance the producer cursor. Relaxed is fine: only the producer thread
    // reads/writes next_publish_, and the per-slot release stamp is what
    // consumers synchronize on, not this counter. Consumers in Backpressure
    // read consumer cursors, never next_publish_.
    next_publish_.store(seq + 1, std::memory_order_relaxed);

    // Wake any futex-blocked consumers. No-op (constexpr-removed) for the
    // spinning strategies. Must come AFTER the release publish so a woken
    // consumer's acquire load observes the new stamp.
    if constexpr (Wait::blocks) {
      // Wake on the per-slot stamp the consumers actually wait on.
      Wait::notify(slot.seq);
    }

    return seq;
  }

  // Sequence the producer will assign next (== count of published messages).
  std::uint64_t produced() const noexcept {
    return next_publish_.load(std::memory_order_acquire);
  }

  // ------------------------------------------------------------------------
  // Consumer cursor registration (Backpressure needs producer-visible cursors)
  // ------------------------------------------------------------------------

  // Claim an inline cursor slot. Returns the cursor id, or max_consumers if
  // full. The cursor's value is the next sequence this consumer will read.
  //
  // BACKPRESSURE REGISTRATION CONTRACT (precondition — read this):
  // A Backpressure consumer MUST register before the producer publishes its
  // (Capacity)-th message. A consumer that registers late starts with cursor
  // value 0 (== "next to read is seq 0"), but the slot for seq 0 may already
  // have been overwritten by a newer lap. The producer would then block forever
  // in await_consumers_past() waiting for this cursor to advance past a sequence
  // whose slot is gone, while the consumer's do_read(0) returns `lapped` —
  // which Backpressure consumers do not handle. Result: producer deadlock.
  // The in-process tests enforce this with a start barrier (all consumers
  // register, THEN the producer starts). A general late-join / dynamic-attach
  // protocol (and the cross-process version of this hazard) is deferred to the
  // discovery/handshake epic shimmy-uud; until then, Backpressure assumes a
  // fixed consumer set joined before the stream starts. Overwrite has no such
  // constraint (late consumers simply resync to the oldest resident sequence).
  std::size_t register_consumer() noexcept {
    for (std::size_t i = 0; i < MaxConsumers; ++i) {
      bool expected = false;
      // acq_rel CAS: claims the slot and publishes value=0 init to the producer.
      if (cursors_[i].in_use.compare_exchange_strong(
              expected, true, std::memory_order_acq_rel,
              std::memory_order_relaxed)) {
        cursors_[i].value.store(0, std::memory_order_release);
        return i;
      }
    }
    return MaxConsumers; // full
  }

  void unregister_consumer(std::size_t id) noexcept {
    if (id >= MaxConsumers) {
      return;
    }
    // Advance this cursor to "infinity" so a Backpressure producer never waits
    // on a departed consumer, then release the slot.
    cursors_[id].value.store(~std::uint64_t{0}, std::memory_order_release);
    cursors_[id].in_use.store(false, std::memory_order_release);
  }

  // ------------------------------------------------------------------------
  // Consumer side — low-level slot access (used by Consumer<> below)
  // ------------------------------------------------------------------------

  slot_type& slot_at(std::uint64_t seq) noexcept { return slots_[seq & mask]; }
  const slot_type& slot_at(std::uint64_t seq) const noexcept {
    return slots_[seq & mask];
  }

  // Publish a consumer's progress so a Backpressure producer can see it.
  void set_cursor(std::size_t id, std::uint64_t value) noexcept {
    if (id < MaxConsumers) {
      // RELEASE: the producer's acquire load of this cursor (await_consumers_
      // past) must not be reordered before the consumer finished reading the
      // slot. This is the lossless-handoff edge for Backpressure.
      cursors_[id].value.store(value, std::memory_order_release);
    }
  }

private:
  // Per-consumer cursor: the next sequence this consumer intends to read.
  // Cache-line isolated so consumer writes don't false-share with each other or
  // with producer state.
  struct alignas(detail::cache_line_size) Cursor {
    std::atomic<std::uint64_t> value;
    std::atomic<bool> in_use;
  };

  // Backpressure: spin until every active consumer cursor is strictly greater
  // than `seq` (i.e. has finished reading the message with that sequence).
  void await_consumers_past(std::uint64_t seq) noexcept {
    std::uint64_t iter = 0;
    for (;;) {
      bool all_clear = true;
      for (std::size_t i = 0; i < MaxConsumers; ++i) {
        // ACQUIRE on in_use so that if we see it active we also see an
        // initialized value (paired with register_consumer's release).
        if (!cursors_[i].in_use.load(std::memory_order_acquire)) {
          continue;
        }
        // ACQUIRE on value: pairs with the consumer's set_cursor release so we
        // observe its progress (and the implied "slot free") correctly.
        if (cursors_[i].value.load(std::memory_order_acquire) <= seq) {
          all_clear = false;
          break;
        }
      }
      if (all_clear) {
        return;
      }
      Spin::pause();
      ++iter;
      (void)iter;
    }
  }

  // --- Layout. Producer-written and consumer-written state are separated onto
  // distinct cache lines to avoid false sharing (DESIGN §3 layout note).

  // Producer cursor: written only by the producer. Its own cache line.
  alignas(detail::cache_line_size) std::atomic<std::uint64_t> next_publish_;

  // Consumer cursors: each is itself cache-line aligned (Cursor) so consumers
  // do not false-share with one another.
  alignas(detail::cache_line_size)
      std::array<Cursor, MaxConsumers> cursors_;

  // The slots. Each Slot is cache-line aligned.
  alignas(detail::cache_line_size) std::array<slot_type, Capacity> slots_;
};

// ----------------------------------------------------------------------------
// Consumer
// ----------------------------------------------------------------------------
// A lightweight, monomorphized reader over a Ring. Holds:
//   * a reference to the ring (in-process; shimmy-52e replaces this with an
//     offset-based view, but the protocol is identical),
//   * its own next-sequence cursor,
//   * the id of its inline cursor slot (for Backpressure visibility),
//   * the sequence/slot of the in-flight zero-copy read for validate().
//
// Zero-copy read contract (DESIGN §2):
//   auto m = c.read();
//   if (c.status() == read_status::ok) {
//     use m in place ...
//     if (!c.validate()) { /* lapped mid-read: discard, will re-read */ }
//   }
template <typename RingT>
class Consumer {
public:
  using slot_type = typename RingT::slot_type;
  static constexpr std::size_t capacity = RingT::capacity;

  explicit Consumer(RingT& ring) noexcept
      : ring_(&ring), next_(0), cursor_id_(RingT::max_consumers) {
    // Only Backpressure rings need a producer-visible cursor; registering for
    // Overwrite is harmless but we skip it to keep cursor slots free.
    if constexpr (std::is_same_v<typename RingT::overflow_policy,
                                 Backpressure>) {
      cursor_id_ = ring_->register_consumer();
    }
  }

  ~Consumer() {
    if constexpr (std::is_same_v<typename RingT::overflow_policy,
                                 Backpressure>) {
      if (cursor_id_ != RingT::max_consumers) {
        ring_->unregister_consumer(cursor_id_);
      }
    }
  }

  Consumer(const Consumer&) = delete;
  Consumer& operator=(const Consumer&) = delete;

  // Attempt to read the next message in sequence WITHOUT blocking.
  // Returns a span viewing the payload in place; check status() for validity.
  std::span<const std::byte> read() noexcept {
    last_status_ = do_read(next_);
    if (last_status_ == read_status::ok) {
      return current_view();
    }
    return {};
  }

  // Block (per the ring's wait strategy) until the next message is available,
  // then return a zero-copy view. Only returns `empty` is impossible here;
  // for Overwrite it may return a `lapped` status (caller resyncs).
  std::span<const std::byte> read_blocking() noexcept {
    using Wait = typename RingT::wait_strategy;
    std::uint64_t iter = 0;
    for (;;) {
      last_status_ = do_read(next_);
      if (last_status_ != read_status::empty) {
        break;
      }
      // Wait for the slot we're looking at to be published.
      const slot_type& slot = ring_->slot_at(next_);
      Wait::wait(slot.seq, next_, iter);
      ++iter;
    }
    if (last_status_ == read_status::ok) {
      return current_view();
    }
    return {};
  }

  // Copying variant: snapshot the payload into `out` (must be >= len bytes).
  // Returns the number of bytes copied (0 on non-ok). Inherently lap-safe: we
  // validate AFTER copying and report 0 if we were lapped mid-copy.
  std::size_t read_copy(void* out, std::size_t out_cap) noexcept {
    last_status_ = do_read(next_);
    if (last_status_ != read_status::ok) {
      return 0;
    }
    const slot_type& slot = ring_->slot_at(read_seq_);
    const std::uint32_t len = slot.len;
    const std::size_t n = (len <= out_cap) ? len : out_cap;
    if (n > 0) {
      std::memcpy(out, slot.payload.data(), n);
    }
    // Validate the copy: if the stamp moved we were lapped while copying and the
    // bytes are torn. Report 0 and let the caller re-read.
    if (!validate()) {
      last_status_ = read_status::lapped;
      return 0;
    }
    commit();
    return n;
  }

  read_status status() const noexcept { return last_status_; }

  std::uint64_t sequence() const noexcept { return read_seq_; }

  // Seqlock read-side validation. Re-load the slot's publish stamp and compare
  // to the sequence we read. Valid iff it is STILL exactly read_seq_ — i.e. the
  // producer neither started overwriting (would show writing_seq) nor finished
  // a lap (would show a newer seq) since do_read captured read_seq_.
  //
  // Memory ordering: the consumer's payload loads are plain loads issued BEFORE
  // this call. We need them ordered-before this second stamp load, otherwise the
  // CPU could hoist the stamp load ahead of a payload load and miss a concurrent
  // overwrite. An acquire LOAD does not give that (acquire only blocks *later*
  // ops from moving up). The correct primitive is an acquire FENCE between the
  // payload reads and the stamp re-load — this is the canonical seqlock read
  // side. The re-load itself can then be relaxed; the fence supplies the order.
  bool validate() const noexcept {
    const slot_type& slot = ring_->slot_at(read_seq_);
    std::atomic_thread_fence(std::memory_order_acquire);
    return slot.seq.load(std::memory_order_relaxed) == read_seq_;
  }

  // Advance past the message just read. For Backpressure this publishes our
  // progress so the producer may reclaim the slot. Call after a successful,
  // validated read (read()/read_blocking() do NOT auto-advance so the caller
  // can validate the zero-copy view first).
  void commit() noexcept {
    next_ = read_seq_ + 1;
    publish_cursor();
  }

  // Skip-ahead resync after a detected lap (Overwrite). Jumps the cursor to the
  // oldest sequence still resident in the ring relative to `produced`.
  void resync(std::uint64_t produced) noexcept {
    // Oldest sequence still in the ring is produced - Capacity (if produced >
    // Capacity), else 0. We resume from there.
    next_ = (produced > capacity) ? (produced - capacity) : 0;
    publish_cursor();
  }

  std::uint64_t next() const noexcept { return next_; }

private:
  std::span<const std::byte> current_view() const noexcept {
    const slot_type& slot = ring_->slot_at(read_seq_);
    return std::span<const std::byte>(slot.payload.data(), slot.len);
  }

  // Core read: examine the slot for sequence `want`.
  read_status do_read(std::uint64_t want) noexcept {
    const slot_type& slot = ring_->slot_at(want);
    // ACQUIRE: pairs with producer's release publish. If stamp == want, the
    // payload+len for `want` are visible.
    const std::uint64_t stamp = slot.seq.load(std::memory_order_acquire);

    if (stamp == want) {
      read_seq_ = want;
      return read_status::ok;
    }
    if (stamp == writing_seq) {
      // The producer is mid-overwrite of this slot RIGHT NOW. Whether it is
      // (re)writing OUR seq (first publish) or lapping us, the bytes are in
      // flux: treat as "nothing readable yet" and retry. The caller's next poll
      // resolves it to ok (our seq landed) or lapped (a newer seq landed).
      return read_status::empty;
    }
    if (stamp == empty_seq || stamp < want) {
      // Not published yet (never-written sentinel, or this slot still holds an
      // OLDER lap that hasn't been overwritten up to `want`). Nothing for us.
      return read_status::empty;
    }
    // stamp > want (and not a sentinel): the producer lapped us — the slot now
    // holds a NEWER message (sequence want + k*Capacity, k>=1). Messages in
    // [want, stamp) that mapped to slots we hadn't reached are lost. Report
    // lapped; caller resyncs. (Only reachable under Overwrite; Backpressure
    // never overwrites an unread slot.)
    return read_status::lapped;
  }

  void publish_cursor() noexcept {
    if constexpr (std::is_same_v<typename RingT::overflow_policy,
                                 Backpressure>) {
      if (cursor_id_ != RingT::max_consumers) {
        ring_->set_cursor(cursor_id_, next_);
      }
    }
  }

  RingT* ring_;
  std::uint64_t next_;       // next sequence we want to read
  std::uint64_t read_seq_{}; // sequence of the in-flight read (for validate)
  std::size_t cursor_id_;    // inline cursor slot id (Backpressure)
  read_status last_status_{read_status::empty};
};

} // namespace shimmy

#endif // SHIMMY_RING_HPP
