# shimmy benchmark baselines

These CSVs are the committed reference the regression gate
(`bench/check_regression.py`) compares fresh runs against.

## THESE NUMBERS ARE MACHINE-SPECIFIC AND REGENERABLE

A baseline is a property of *the machine + toolchain it was recorded on*, not an
absolute truth. The committed baseline here was recorded on:

- **CPU:** AMD Ryzen AI 9 HX 370 (12 cores / 24 threads, single NUMA node, x86-64)
- **Governor:** `powersave`, **turbo:** on  → flagged `noisy=1` in the CSV
- **Toolchain:** clang 18.1.8, `-DCMAKE_BUILD_TYPE=Release`, pinned Nix devshell
- **Policy/threads:** throughput = Overwrite + Spin readers; latency = Backpressure,
  producer/consumer pinned to distinct physical cores, 2µs producer pacing.

Because it was recorded under a **noisy CPU config** (no performance governor, no
core isolation), the tail latencies (p99.9+) are *indicative, not authoritative*
— see the `noisy` column and the harness's stderr banner. The median path
(p50/p90/p99) and the throughput min-time figures are far more stable.

## Regenerate when

- You change the machine/toolchain you gate on.
- The numbers drift legitimately (a real optimization or a hardware change).
- You move to a quiet, pinned machine (then re-record with `noisy=0` and gate
  `--strict`).

```bash
nix develop --command bash -c '
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build &&
  ./build/bench/shimmy_throughput_bench --messages=10000000 --reps=4 \
      --csv=bench/baselines/throughput.csv &&
  ./build/bench/shimmy_latency_bench --samples=300000 --gap_ns=2000 \
      --csv=bench/baselines/latency.csv'
```

For an AUTHORITATIVE baseline (worth gating hard on), record on a quiet machine:

```bash
# requires privileges
sudo cpupower frequency-set -g performance
echo 1 | sudo tee /sys/devices/system/cpu/cpufreq/boost   # or no_turbo for Intel
# ideally boot with isolcpus= to isolate the producer/consumer cores
```

## Gate semantics

See the header of `bench/check_regression.py`. In short: throughput drop >20% or
p99.9 latency increase >50% is a regression; the gate is **advisory** in CI
(noisy shared runners) and **hard** (`--strict`) when a maintainer re-checks on a
pinned machine. A regression detected when the *current* run is `noisy=1` is
downgraded to a warning unless `--ignore-noise` is given.
