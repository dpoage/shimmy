#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# shimmy benchmark regression gate (bead shimmy-7d8).
#
# Compares a fresh benchmark run against a COMMITTED baseline and flags
# meaningful regressions. This is the CI "data doesn't lie" gate: a perf claim
# that silently rots is worse than no claim.
#
# Two CSV kinds are understood (schema documented in bench/latency_bench.cpp and
# bench/throughput_bench.cpp):
#   * throughput rows  -> regression = throughput DROP beyond tolerance.
#   * latency rows     -> regression = p99.9 latency INCREASE beyond tolerance.
#
# DESIGN DECISIONS (recorded here because the gate's behavior is a deliverable):
#
#   * KEYING. Rows are matched by their dimension columns, not by line order:
#       throughput key = (block_size, consumers)
#       latency    key = (wait_strategy, block_size)
#     so reordering or adding cells never causes false matches.
#
#   * TOLERANCE. Benchmarks are NOISY (esp. without a performance governor /
#     isolated cores). A tight threshold would flap. Defaults:
#       throughput: fail if current < baseline * (1 - 0.20)   (>20% slower)
#       latency   : fail if current > baseline * (1 + 0.50)   (>50% worse p99.9)
#     Latency tails are far noisier than throughput, hence the looser bound.
#     Both are overridable via --throughput-tol / --latency-tol.
#
#   * HARD vs ADVISORY. The gate is ADVISORY BY DEFAULT (exit 0, print findings)
#     and HARD only with --strict (exit 1 on regression). Rationale: CI runs on
#     shared GitHub-hosted runners with NO performance governor, NO core
#     isolation, and noisy neighbors — exactly the config this harness flags as
#     untrustworthy for tails. Making the gate hard there would flap and erode
#     trust ("data doesn't lie" dies the moment the gate cries wolf). So CI runs
#     it advisory (surfaces drift in the log, never blocks a PR on runner
#     noise); a maintainer regenerating the baseline on a PINNED, quiet machine
#     runs it --strict to actually gate. The baseline file itself is marked
#     machine-specific and regenerable (see baselines/README.md).
#
#   * NOISY-RUN GUARD. If the CURRENT run was recorded under a noisy CPU config
#     (the harness sets noisy=1), the gate downgrades any regression to a
#     warning even under --strict UNLESS --ignore-noise is passed. This stops a
#     frequency-scaling blip on a shared runner from failing the build, while
#     still printing the delta.
#
# Usage:
#   check_regression.py --baseline B.csv --current C.csv [--strict]
#                       [--throughput-tol 0.20] [--latency-tol 0.50]
#                       [--ignore-noise]

import argparse
import csv
import sys


def read_csv(path):
    """Return list of dict rows; tolerant of the leading comment banner lines
    the C++ harness prints to stdout (those start with '#' and are not part of
    the CSV file, but we strip them defensively if a redirected log is fed in)."""
    rows = []
    with open(path, newline="") as f:
        lines = [ln for ln in f if not ln.lstrip().startswith("#") and ln.strip()]
    reader = csv.DictReader(lines)
    for r in reader:
        rows.append(r)
    return rows


def index_throughput(rows):
    out = {}
    for r in rows:
        if r.get("kind") != "throughput":
            continue
        key = (r["block_size"], r["consumers"])
        out[key] = r
    return out


def index_latency(rows):
    out = {}
    for r in rows:
        if r.get("kind") != "latency":
            continue
        key = (r["wait_strategy"], r["block_size"])
        out[key] = r
    return out


def current_is_noisy(rows):
    return any(r.get("noisy") == "1" for r in rows)


def main():
    ap = argparse.ArgumentParser(description="shimmy benchmark regression gate")
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--current", required=True)
    ap.add_argument("--throughput-tol", type=float, default=0.20,
                    help="fractional throughput drop that counts as a regression")
    ap.add_argument("--latency-tol", type=float, default=0.50,
                    help="fractional p99.9 increase that counts as a regression")
    ap.add_argument("--strict", action="store_true",
                    help="exit non-zero on a regression (default: advisory)")
    ap.add_argument("--ignore-noise", action="store_true",
                    help="do not downgrade regressions when the current run was "
                         "recorded under a noisy CPU config")
    args = ap.parse_args()

    base = read_csv(args.baseline)
    cur = read_csv(args.current)

    regressions = []   # (descr, baseline_val, current_val, pct)
    improvements = []
    missing = []

    # --- Throughput ---
    b_tp = index_throughput(base)
    c_tp = index_throughput(cur)
    for key, brow in b_tp.items():
        crow = c_tp.get(key)
        if crow is None:
            missing.append(f"throughput cell block={key[0]} consumers={key[1]} "
                           f"absent from current run")
            continue
        bval = float(brow["throughput_msgs_per_sec"])
        cval = float(crow["throughput_msgs_per_sec"])
        if bval <= 0:
            continue
        pct = (cval - bval) / bval
        descr = f"throughput block={key[0]}B consumers={key[1]}"
        if cval < bval * (1.0 - args.throughput_tol):
            regressions.append((descr, bval, cval, pct))
        elif pct > 0.10:
            improvements.append((descr, bval, cval, pct))

    # --- Latency (p99.9) ---
    b_lat = index_latency(base)
    c_lat = index_latency(cur)
    for key, brow in b_lat.items():
        crow = c_lat.get(key)
        if crow is None:
            missing.append(f"latency strategy={key[0]} block={key[1]} absent "
                           f"from current run")
            continue
        bval = float(brow["p999_ns"])
        cval = float(crow["p999_ns"])
        if bval <= 0:
            continue
        pct = (cval - bval) / bval
        descr = f"latency {key[0]} p99.9 block={key[1]}B"
        if cval > bval * (1.0 + args.latency_tol):
            regressions.append((descr, bval, cval, pct))
        elif pct < -0.10:
            improvements.append((descr, bval, cval, pct))

    print("=== shimmy regression gate ===")
    print(f"baseline: {args.baseline}")
    print(f"current : {args.current}")
    print(f"tolerances: throughput -{args.throughput_tol*100:.0f}%  "
          f"latency +{args.latency_tol*100:.0f}%")
    noisy = current_is_noisy(cur)
    print(f"current run noisy CPU config: {'YES' if noisy else 'no'}")
    print()

    for descr, b, c, pct in improvements:
        print(f"  [improved] {descr}: {b:.3g} -> {c:.3g} ({pct*100:+.1f}%)")
    for m in missing:
        print(f"  [missing ] {m}")
    if not regressions:
        print("  no regressions beyond tolerance.")
    for descr, b, c, pct in regressions:
        print(f"  [REGRESS ] {descr}: {b:.3g} -> {c:.3g} ({pct*100:+.1f}%)")

    print()
    if not regressions:
        print("RESULT: PASS")
        return 0

    if not args.strict:
        print("RESULT: ADVISORY — regressions found but gate is advisory "
              "(pass --strict to fail the build).")
        return 0

    if noisy and not args.ignore_noise:
        print("RESULT: WARN — regressions found but the current run was under a "
              "NOISY CPU config; downgraded to a warning. Re-run on a pinned, "
              "quiet machine, or pass --ignore-noise to enforce.")
        return 0

    print("RESULT: FAIL — regression(s) beyond tolerance under --strict.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
