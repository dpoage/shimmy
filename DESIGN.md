# shimmy — Design

> Single-host, shared-memory, SPMC messaging for massive throughput and
> excellent, well-bounded latency. Linux-only, modern C++20, header-only library
> with compiled benchmark/analytics targets.
>
> **Guiding principle: speculation is useless, data doesn't lie.** Every
> performance claim in this document is a *hypothesis* until a benchmark in
> `bench/` confirms it on real hardware. Targets are acceptance gates, measured
> — never asserted.

---

## 1. Problem statement

Provide a shared-memory message channel where **one producer** fans out to
**many consumers** on a **single host**, with:

- **Massive throughput** — headline target ≥10M msgs/sec, single producer, 64B.
- **Excellent, well-bounded latency** — bounded p99.9 tail, where the bound is a
  property of the chosen wait strategy (see §6), benched against each
  implementation rather than asserted as one universal number.
- **Variable but mostly-small payloads**, organized into **channels**.

Non-goals (phase 1): cross-host/network transport, multi-producer, multi-NUMA,
ARM64. These are explicitly deferred (see §9).

---

## 2. Core model: the Channel

A **Channel** is the unit of communication. Its defining property:

> A channel has a **fixed block size for its entire lifecycle**, chosen at
> **compile time**.

- Small-message channels use small blocks (e.g. `Channel<256>`).
- Frame/image channels use large blocks (e.g. `Channel<2*1024*1024>`).
- A producer and its consumers all agree on the same instantiation.

This is a **fixed-size-slot ring buffer**:

```
 ring of Capacity blocks, each BlockSize bytes
┌──────────┬──────────┬──────────┬─────┬──────────┐
│ block 0  │ block 1  │ block 2  │ ... │ block N-1│
└──────────┴──────────┴──────────┴─────┴──────────┘
   seq=42      seq=43

block layout:  [ u32 len ][ payload bytes ... ][ unused tail ]
```

- **Variable-but-bounded payload**: a message ≤ `BlockSize - header` writes its
  real length + bytes; the tail of the block is unused. This is the deliberate
  memory cost of fixed blocks — paid in RAM, *not* in latency. It sidesteps the
  wrap-handling and framing CPU cost of a variable-length (bip) buffer entirely.
- **Power-of-2 `Capacity`** → ring index is `seq & (Capacity-1)`, a mask, not a
  modulo.

### Publish/consume protocol (sketch — to be validated)

- Producer writes payload + length into block `seq & mask`, then **publishes**
  by storing the sequence number with release semantics. The published sequence
  number is the single synchronization point.
- Consumer loads the slot's sequence with acquire semantics; if it matches the
  sequence it expects, the data for that slot is visible.
- **Zero-copy read (default):** `read()` returns a `std::span<const std::byte>`
  viewing the payload *in place* in shared memory. After the consumer is done
  using the view, `validate()` re-loads the slot sequence; if it changed, the
  consumer was lapped while reading and must discard/retry. Copying is opt-in
  (`read_copy()`), for when the consumer needs to retain the data.

```cpp
auto msg = consumer.read();          // std::span<const std::byte> into shmem
// ... use msg in place, zero copy ...
if (!consumer.validate()) {          // lapped mid-read? -> stale, retry
    continue;
}
```

x86-64 has a TSO memory model, which makes acquire/release cheap (no fences on
the common path). Correctness of the exact protocol is a phase-1 deliverable
backed by tests (incl. TSan / a stress harness), not assumed here.

---

## 3. Compile-time configuration axes

All of these are template parameters / tag types that **collapse to constants**
the optimizer sees, leaving a straight-line hot path: a sequence load, an
acquire, a `memcpy` (or a span construction for zero-copy).

| Axis | Mechanism | Why compile-time |
|---|---|---|
| **Block size** | `std::size_t BlockSize` (NTTP) | Slot stride; enables `static_assert` on alignment & cache-line padding |
| **Capacity** | `std::size_t Capacity`, power-of-2 | Index becomes mask `& (N-1)`, not `%` |
| **Overflow policy** | `Overwrite` / `Backpressure` tag type | The two have *different* slot-lifetime invariants; tag dispatch keeps each branch-free |
| **Max consumers** | bounded `N` (inline cursors) or dynamic | Fixed N → consumer cursors live inline in the header, no indirection |
| **Wait strategy** | `Spin` / `SpinThenYield` / `Futex` | Latency-vs-CPU tradeoff; owned by the consumer, picked at compile time |

### Overflow policy — the two invariants

- **`Overwrite` (never block producer):** producer free-runs and may lap slow
  consumers. A lapped consumer detects a sequence gap, reports loss, resyncs.
  Throughput-optimal, bounded producer latency, lossy under pressure. Classic
  for telemetry / market-data fan-out.
- **`Backpressure` (lossless):** producer waits for the slowest consumer cursor
  before reusing a slot. Lossless, but one slow consumer stalls everyone —
  producer latency is coupled to the worst consumer. **Not** independently
  bounded.

Both are supported, selected per-channel at compile time. This roughly doubles
the correctness/test surface; the cost is acknowledged and accepted.

---

## 4. Scope (phase 1)

1. **Core SPMC ring** — templated, in-process first (no shmem dependency for
   the unit under test). Producer write, consumer read, sequencing, both
   overflow policies, all wait strategies.
2. **Shared-memory segment lifecycle** — `shm_open`/`mmap`, header
   magic + version, alignment, hugepage (`MAP_HUGETLB`) option, teardown/unlink
   ownership.
3. **Benchmark + analytics harness** — the "data doesn't lie" backbone. Built
   *alongside* the core, not after. HdrHistogram latency (p50…p99.99),
   throughput sweeps, perf-counter capture, CSV output, CI regression gate.

Deferred to a later epic (see §9): producer/consumer discovery + handshake +
crash-safety.

---

## 5. Targets (acceptance gates — measured, not asserted)

| Target | Value | Notes |
|---|---|---|
| **Throughput** | ≥ 10M msgs/sec | single producer, 64B headline, ~100ns/msg budget |
| **Latency** | **tiered by wait strategy** | see §6; each tier benched against its own impl |
| **Bench msg sizes** | sweep 64B / 256B / 1KB / 4KB | headline @64B, **full curve published** |

The throughput target leaves real headroom (~100ns/msg) for variable payloads
and multi-consumer fan-out, rather than chasing a number that forces batching
and tiny blocks. If measurement shows easy headroom, we raise the gate from
data.

---

## 6. Latency tiers by wait strategy

We do **not** pick one latency ceiling. Each wait strategy has its own bound,
benched against its own implementation and published:

| Wait strategy | p99.9 target | Cost / tradeoff |
|---|---|---|
| `Spin` | < 200 ns | busy-spin, no syscalls, NUMA-local; burns a core; tightest tail |
| `SpinThenYield` | < 1 µs | spin briefly then `sched_yield`; robust to jitter; strong default |
| `Futex` | < 10 µs | block on futex; CPU-friendly under oversubscription; easiest to guarantee |

Each tier's number is a hypothesis until `bench/` confirms it on the target
hardware. The deliverable is the *measured tail per strategy*, not a single
asserted ceiling.

---

## 7. Deliverable shape

- **Header-only library** under `include/shimmy/` — templated, max inlining,
  consumers just add the include path.
- **Compiled benchmark/analytics targets** under `bench/` — HdrHistogram, perf
  counters, CSV/plots, CI regression gate.
- **Tests** under `tests/` — correctness, overflow-policy invariants, lap
  detection, concurrency stress (TSan).

---

## 8. Environment & constraints

- **Language:** C++20.
- **Build/dev:** Nix devshell (`flake.nix`) — pinned clang/gcc, CMake (or Meson),
  HdrHistogram, benchmark deps.
- **OS:** Linux only.
- **Hardware (phase 1):** x86-64, **single NUMA node**. TSO memory model assumed
  for the common path; atomics still expressed correctly via C++ memory order so
  the door to weaker models stays open.

---

## 9. Deferred / future epics

- Producer/consumer **discovery + handshake**: consumer registry in the shm
  header, cursor slot claim/release, layout agreement.
- **Crash-safety**: dead-consumer detection (PID liveness / heartbeat), cursor
  reclamation when a consumer dies holding state.
- **Multi-NUMA** placement & cross-node coherence measurement.
- **ARM64** support (forces rigorous fences; doubles the test matrix).
- Multi-producer (MPMC/MPSC) variants.

---

## 10. Open questions (to resolve with data, not debate)

- Exact slot-publish protocol & the cache-line layout that minimizes coherence
  traffic for N consumers — settle by benchmarking candidates.
- Whether inline-fixed vs. dynamic consumer cursors measurably matters at the
  target consumer counts.
- Hugepage impact on the latency tail — measure with and without `MAP_HUGETLB`.
- Where `Backpressure` mode's producer-latency coupling becomes unacceptable —
  characterize the slow-consumer cliff.
