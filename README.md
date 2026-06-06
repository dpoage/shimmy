# shimmy

[![CI](https://github.com/dpoage/shimmy/actions/workflows/ci.yml/badge.svg)](https://github.com/dpoage/shimmy/actions/workflows/ci.yml)

**One producer, many consumers, one machine — at memory-bus speed, with a
latency tail you can name.** shimmy is a header-only C++20 library that fans a
single producer out to many consumers over shared memory on one Linux host. It's
built for a firehose of mostly-small messages and a handful of readers that all
need to see them *now*: telemetry, market-data fan-out, frame pipelines.

It's a sharp tool for one job, not a messaging framework. No broker, no network,
no multi-producer write path, no persistence, no schema layer. Every axis that
isn't on the hot path was left off so the path that remains is a straight line.

Every number below comes from a benchmark in `bench/`, run on real hardware,
with the charts regenerated from committed CSVs — and every chart states its own
measurement caveats. Claims are earned by data, not asserted.

---

## Throughput

Design target (DESIGN §5): **≥ 10M msgs/sec** for a single producer at 64-byte
messages. The real question was how much headroom is actually there.

![Throughput vs message size](assets/throughput_vs_size.svg)

- **Headline** — one producer fanning out to one concurrent consumer hits
  **151 M msgs/sec at 64B**, ~15× the target, and clears the gate across the
  whole size sweep.
- **Publish ceiling** — with no readers attached the producer's hot path does
  **~1.10 B msgs/sec at 64B**. The gap between the two lines *is* the cost of
  fan-out: the producer's stores being pulled into a reader's cache.
- **Crossover** — both lines bend down at the right edge, where shimmy stops
  being rate-bound and becomes bandwidth-bound (~tens of GiB/s at 4KB). Small
  messages are limited by per-message work, big ones by raw bandwidth. The chart
  shows the crossover instead of asserting it.

Full sweep (64B/256B/1KB/4KB × 0/1/2/4 consumers):
[`bench/baselines/throughput.csv`](bench/baselines/throughput.csv).

---

## Latency

shimmy won't quote a single latency ceiling — the right one depends on how a
consumer chooses to *wait*. Each wait strategy is its own tier (DESIGN §6),
benched on its own.

![Latency tails per wait strategy](assets/latency_tails.svg)

| Wait strategy | p50 | p99 | p99.9 | Budget |
|---|---|---|---|---|
| `Spin` — busy-spin, tightest tail, burns a core | 30 ns | 41 ns | 240 ns | < 200 ns |
| `SpinThenYield` — spin then `sched_yield`, default | 30 ns | 41 ns | 251 ns | < 1 µs |
| `Futex` — block, CPU-friendly under oversubscription | 692 ns | 1 964 ns | 2 415 ns | < 10 µs |

The median path is rock-solid run to run, and every tier sits at or near its
p99.9 budget. The committed baseline is measured on a `noisy=1` host, so p99.9+
is indicative until the turbo-off re-measure (`shimmy-cvs`) lands — the chart and
[`bench/baselines/latency.csv`](bench/baselines/latency.csv) carry the details.

> **Bench host:** AMD Ryzen AI 9 HX 370 (12c/24t, single NUMA node), clang 18.1.8
> `Release`, pinned Nix devshell.

---

## How it works

A **channel** has one defining property: its **block size is fixed for the whole
lifecycle and chosen at compile time**. Under the hood it's a fixed-size-slot
ring buffer — a power-of-two array of equal blocks, each `[u32 len][payload][slack]`.
A short message writes its real length and bytes; the rest is slack. That slack
is a deliberate trade: paid in RAM, *not* latency, and in exchange the hot path
never touches the wrap-handling and framing a variable-length buffer needs.
Power-of-two capacity makes the ring index a mask (`seq & (Capacity-1)`), never a
modulo.

Everything that *could* be a runtime decision is instead a compile-time template
axis that collapses to a constant the optimizer can see:

| Axis | Parameter | Why compile-time |
|---|---|---|
| **Block size** | `std::size_t BlockSize` | slot stride; `static_assert` on cache-line alignment |
| **Capacity** | `std::size_t Capacity` (power of 2) | index becomes a mask, not a `%` |
| **Overflow policy** | `Overwrite` \| `Backpressure` | different slot-lifetime invariants; tag dispatch keeps each branch-free |
| **Max consumers** | `std::size_t MaxConsumers` (default 16) | fixed N → consumer cursors live inline in the header |
| **Wait strategy** | `Spin` \| `SpinThenYield<N>` \| `Futex` | the latency-vs-CPU tier, owned by the consumer |

The **overflow policy** matters most. Under `Overwrite` the producer free-runs
and may lap a slow consumer — throughput-optimal, lossy under pressure (the
lapped consumer detects the gap, reports loss, resyncs). Under `Backpressure` the
producer waits for the slowest consumer before reusing a slot — lossless, but one
slow reader stalls everyone. Both ship; pick per channel.

### Zero-copy reads, made safe by a seqlock

The producer writes the payload into slot `seq & mask`, then **publishes** by
storing the sequence number with release semantics; the consumer loads it with
acquire and, if it matches, the payload is visible. On x86-64's TSO model both
are plain `MOV`s, so correctness on weaker models is free and the common path
pays nothing.

That makes the default `read()` zero-copy: it returns a `std::span<const
std::byte>` viewing the bytes in place. Under `Overwrite` the producer could
overwrite those bytes mid-read (the classic benign seqlock race). shimmy closes
it with a two-phase publish (a `writing_seq` marker) plus `validate()`: after
you're done with the view, `validate()` re-checks the stamp and you discard a
torn read and retry. Nothing torn is ever committed — **TSan-verified across
millions of forced laps**. Under `Backpressure` the slot is stable; no race.

```cpp
#include <shimmy/ring.hpp>

using namespace shimmy;

// 64-byte blocks, 1024 slots, Overwrite policy, Spin readers — all compile-time.
Ring<64, 1024, Overwrite, /*MaxConsumers=*/16, Spin> ring;

// Producer (single producer; not thread-safe to call concurrently).
ring.publish(hello, len);

// Consumer (many, across threads or processes).
Consumer consumer(ring);
auto msg = consumer.read();                 // zero-copy view into shmem
if (consumer.status() == read_status::ok && consumer.validate()) {
  use(msg);                                 // validate(): were we lapped mid-read?
  consumer.commit();                        // advance to the next message
}

// Copying read: snapshots into a buffer, inherently lap-safe (returns 0 if torn).
std::byte buf[64];
std::size_t n = consumer.read_copy(buf, sizeof(buf));
```

The same pointer-free `Ring` layout backs the cross-process path in
`shm_segment.hpp` (`shm_open`/`mmap`, versioned header, optional `MAP_HUGETLB`):
standard-layout, all cross-thread state in fixed-width atomics, so the bytes are
identical across processes mapping the segment at different addresses.

See [DESIGN.md](DESIGN.md) for the full rationale.

---

## Status

Phase 1 (epic `shimmy-t2d`) is **complete and verified** on `master`: core SPMC
ring, both overflow policies, all three wait strategies, the shared-memory
segment lifecycle, and the bench/analytics harness (HdrHistogram latency,
throughput sweep, CSV, CI regression gate) — all in and green.

CI (`.github/workflows/ci.yml`) is green: a **clang + gcc** matrix (build +
`ctest` + bench smoke), an advisory **bench** regression gate (shared runners are
noisy), and a separate **TSan** job. Testing: **26 GoogleTest cases** pass under
both toolchains plus a clean TSan run; library-header coverage is **84.99%
lines** via LLVM source-based coverage (`SHIMMY_ENABLE_COVERAGE`).

**Deferred (open beads, not on `master`):**

- `shimmy-cvs` — authoritative tail re-measure on a quiet, turbo-off,
  core-isolated host.
- `shimmy-uud` — producer/consumer discovery, handshake & crash-safety (dynamic
  late-join, dead-consumer reclamation). Today `Backpressure` assumes a fixed
  consumer set joined before the stream starts.
- `shimmy-qui` / `shimmy-o93` — batch-publish/consume latency exploration on a
  branch (early finding: +40–60 ns median buys a tighter heavy-load tail).

**Out of scope by design** — multi-producer writes, cross-host/network
transport, a broker, persistence/replay, built-in serialization. These are the
boundary that lets the in-scope work stay fast. Need them? shimmy is the wrong
tool, deliberately.

Version `0.0.0`; the API is not yet stable.

---

## Quick start

Everything runs inside the **pinned Nix devshell** so the toolchain — and the
numbers — are reproducible. The library itself is header-only: a consumer just
adds `include/` to its path and `#include <shimmy/ring.hpp>`.

```bash
nix develop                                            # pinned clang 18 + gcc 13, cmake, ninja, gtest, ...

cmake -S . -B build -G Ninja && cmake --build build    # clang is the default
ctest --test-dir build --output-on-failure             # 26 tests

# Reproduce the numbers and regenerate the charts:
cmake -S . -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-bench
./build-bench/bench/shimmy_throughput_bench --messages=10000000 --reps=4 --csv=bench/baselines/throughput.csv
./build-bench/bench/shimmy_latency_bench    --samples=300000 --gap_ns=2000 --csv=bench/baselines/latency.csv
python3 bench/plot.py                                  # CSVs -> assets/*.svg

# Must compile cleanly under gcc too:
cmake -S . -B build-gcc -G Ninja -DCMAKE_CXX_COMPILER=g++ && cmake --build build-gcc
```

CMake options: `SHIMMY_BUILD_TESTS` (ON), `SHIMMY_BUILD_BENCH` (ON),
`SHIMMY_ENABLE_TSAN` (OFF), `SHIMMY_ENABLE_COVERAGE` (OFF, clang only).

---

## License

Apache-2.0. Source headers carry `SPDX-License-Identifier: Apache-2.0`.
