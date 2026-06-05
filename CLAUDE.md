# Project Instructions for AI Agents

This file provides instructions and context for AI coding agents working on this project.

<!-- BEGIN BEADS INTEGRATION v:1 profile:minimal hash:ca08a54f -->
## Beads Issue Tracker

This project uses **bd (beads)** for issue tracking. Run `bd prime` to see full workflow context and commands.

### Quick Reference

```bash
bd ready              # Find available work
bd show <id>          # View issue details
bd update <id> --claim  # Claim work
bd close <id>         # Complete work
```

### Rules

- Use `bd` for ALL task tracking — do NOT use TodoWrite, TaskCreate, or markdown TODO lists
- Run `bd prime` for detailed command reference and session close protocol
- Use `bd remember` for persistent knowledge — do NOT use MEMORY.md files

## Session Completion

**When ending a work session**, you MUST complete ALL steps below. Work is NOT complete until `git push` succeeds.

**MANDATORY WORKFLOW:**

1. **File issues for remaining work** - Create issues for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **PUSH TO REMOTE** - This is MANDATORY:
   ```bash
   git pull --rebase
   bd dolt push
   git push
   git status  # MUST show "up to date with origin"
   ```
5. **Clean up** - Clear stashes, prune remote branches
6. **Verify** - All changes committed AND pushed
7. **Hand off** - Provide context for next session

**CRITICAL RULES:**
- Work is NOT complete until `git push` succeeds
- NEVER stop before pushing - that leaves work stranded locally
- NEVER say "ready to push when you are" - YOU must push
- If push fails, resolve and retry until it succeeds
<!-- END BEADS INTEGRATION -->


## Build & Test

All builds run inside the **pinned Nix devshell** so the toolchain (and therefore
benchmark numbers) is reproducible. Do not build against the host toolchain when
you care about numbers.

```bash
# Enter the devshell (pinned clang 18 + gcc 13, cmake, ninja, gtest,
# google-benchmark, HdrHistogram_c, perf, gdb). nixpkgs is pinned in flake.lock.
nix develop

# --- inside the devshell (or wrap each line in: nix develop --command bash -c '...') ---

# Configure + build (clang is the devshell default; pass g++ for the gcc build)
cmake -S . -B build -G Ninja
cmake --build build

# Run tests
ctest --test-dir build --output-on-failure

# Run the benchmark smoke target
./build/bench/shimmy_bench

# Build with the gcc toolchain instead (the project must compile under both)
cmake -S . -B build-gcc -G Ninja -DCMAKE_CXX_COMPILER=g++
cmake --build build-gcc

# ThreadSanitizer build (tests only; bench disabled)
cmake -S . -B build-tsan -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
  -DSHIMMY_ENABLE_TSAN=ON -DSHIMMY_BUILD_BENCH=OFF
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure
```

One-liner used by CI / for a quick full check:

```bash
nix develop --command bash -c '
  cmake -S . -B build -G Ninja &&
  cmake --build build &&
  ctest --test-dir build --output-on-failure &&
  ./build/bench/shimmy_bench --benchmark_min_time=0.05s'
```

CMake options: `SHIMMY_BUILD_TESTS` (default ON), `SHIMMY_BUILD_BENCH`
(default ON), `SHIMMY_ENABLE_TSAN` (default OFF). CI lives in
`.github/workflows/ci.yml` (clang + gcc matrix, plus a TSan job).

## Architecture Overview

shimmy is a single-host, shared-memory, **SPMC** (single-producer /
multi-consumer) messaging library in C++20. Linux-only, x86-64. See `DESIGN.md`
for the full rationale. Layout:

- `include/shimmy/` — the **header-only** library (templated, max inlining;
  consumers just add the include path). This is the product.
- `tests/` — compiled correctness + concurrency (TSan) tests, GoogleTest.
- `bench/` — compiled benchmark/analytics targets, Google Benchmark +
  HdrHistogram. The "data doesn't lie" backbone (throughput sweeps, latency
  tails, CSV, CI regression gate) is built out here.

## Conventions & Patterns

- **C++20**, no compiler extensions (`-std=c++20`, not `gnu++20`).
- Library code is header-only under `include/shimmy/`; never add a compiled
  source file to the library — benchmarks/tests are the only compiled targets.
- Dependencies come from the Nix flake, **not** fetched at CMake configure time,
  for reproducibility. HdrHistogram_c is pinned in `flake.nix` (it is not in
  nixpkgs); everything else comes from the pinned nixpkgs.
- Must compile cleanly under **both** clang and gcc.
