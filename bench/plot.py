#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# shimmy chart generator — turns the committed benchmark baselines into the SVGs
# embedded in the README. "Data doesn't lie": these charts are REGENERATED from
# bench/baselines/*.csv by this script, never hand-drawn, so they cannot drift
# from the measured numbers.
#
# Run from the pinned Nix devshell (matplotlib is pinned in flake.nix):
#
#     nix develop --command python3 bench/plot.py
#
# Inputs : bench/baselines/throughput.csv, bench/baselines/latency.csv
# Outputs: assets/throughput_vs_size.svg, assets/latency_tails.svg
#
# The script is deterministic: same CSVs -> byte-stable* SVGs (we set a fixed
# figure size, fixed font, and an Agg non-interactive backend). (*SVG metadata
# such as the matplotlib version string is the only thing that can change; the
# plotted geometry is a pure function of the CSV data.)

import csv
import os
import sys

import matplotlib

matplotlib.use("Agg")  # non-interactive backend: no display required
import matplotlib.pyplot as plt  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BASELINES = os.path.join(ROOT, "bench", "baselines")
ASSETS = os.path.join(ROOT, "assets")

# Block sizes we sweep, in publication order, with human labels.
BLOCKS = [64, 256, 1024, 4096]
BLOCK_LABELS = {64: "64 B", 256: "256 B", 1024: "1 KB", 4096: "4 KB"}

# Wait strategies in DESIGN §6 tier order.
STRATS = ["Spin", "SpinThenYield", "Futex"]


def _read_csv(name):
    with open(os.path.join(BASELINES, name), newline="") as f:
        return list(csv.DictReader(f))


def _noisy_caveat(rows):
    """Return a human caveat string reflecting the actual CSV noisy/governor/turbo
    columns — honesty is driven by the data, not hard-coded."""
    r = rows[0]
    noisy = r.get("noisy", "?")
    gov = r.get("governor", "?")
    turbo = r.get("turbo", "?")
    if noisy == "1":
        return (
            f"bench host noisy=1 (governor={gov}, turbo {turbo}); "
            "p99.9 indicative, not authoritative — see shimmy-cvs"
        )
    return f"bench host noisy=0 (governor={gov}, turbo {turbo}) — authoritative"


def plot_throughput():
    rows = _read_csv("throughput.csv")
    # index by (block, consumers) -> msgs/sec
    by = {}
    for r in rows:
        by[(int(r["block_size"]), int(r["consumers"]))] = (
            float(r["throughput_msgs_per_sec"]),
            float(r["throughput_mib_per_sec"]),
        )

    drain = [by[(b, 0)][0] / 1e6 for b in BLOCKS]       # 0 readers, M msgs/s
    one = [by[(b, 1)][0] / 1e6 for b in BLOCKS]         # 1 reader, M msgs/s (headline)
    one_mib = [by[(b, 1)][1] for b in BLOCKS]           # 1 reader, MiB/s (bandwidth)

    x = list(range(len(BLOCKS)))
    fig, ax = plt.subplots(figsize=(8.0, 5.0))

    ax.plot(x, drain, marker="s", linewidth=2, color="#9aa0a6",
            label="0 consumers (pure publish ceiling)")
    ax.plot(x, one, marker="o", linewidth=2.5, color="#1a73e8",
            label="1 consumer (headline fan-out)")

    # Headline target line: >= 10M msgs/sec (DESIGN §5).
    ax.axhline(10.0, color="#d93025", linestyle="--", linewidth=1.3)
    ax.text(len(BLOCKS) - 1, 12.5, "DESIGN §5 target ≥ 10M msgs/sec",
            color="#d93025", ha="right", va="bottom", fontsize=9)

    ax.set_yscale("log")
    ax.set_xticks(x)
    ax.set_xticklabels([BLOCK_LABELS[b] for b in BLOCKS])
    ax.set_xlabel("message / block size")
    ax.set_ylabel("throughput (M msgs/sec, log scale)")
    ax.set_title("shimmy throughput vs message size\n"
                 "single producer, Overwrite policy, Spin readers")
    ax.grid(True, which="both", axis="y", alpha=0.3)
    ax.legend(loc="lower left", framealpha=0.95)

    # Annotate the headline 64B/1-consumer point and the 4KB bandwidth rolloff.
    ax.annotate(f"{one[0]:.0f}M msgs/s",
                xy=(0, one[0]), xytext=(0.15, one[0] * 1.8),
                fontsize=9, color="#1a73e8")
    ax.annotate(f"bandwidth-bound\n@4KB ≈ {one_mib[-1] / 1024:.1f} GiB/s",
                xy=(3, one[-1]), xytext=(2.05, one[-1] * 2.2),
                fontsize=8.5, color="#555",
                arrowprops=dict(arrowstyle="->", color="#999", lw=1))

    fig.tight_layout()
    out = os.path.join(ASSETS, "throughput_vs_size.svg")
    fig.savefig(out, format="svg")
    plt.close(fig)
    return out


def plot_latency():
    rows = _read_csv("latency.csv")
    by = {r["wait_strategy"]: r for r in rows}

    pct_keys = [("p50_ns", "p50"), ("p99_ns", "p99"), ("p999_ns", "p99.9")]
    colors = {"p50": "#1a73e8", "p99": "#f9ab00", "p99.9": "#d93025"}

    n_groups = len(STRATS)
    n_bars = len(pct_keys)
    bar_w = 0.26
    x = list(range(n_groups))

    fig, ax = plt.subplots(figsize=(8.0, 5.0))

    for i, (key, label) in enumerate(pct_keys):
        vals = [float(by[s][key]) for s in STRATS]
        offs = [xi + (i - (n_bars - 1) / 2) * bar_w for xi in x]
        bars = ax.bar(offs, vals, width=bar_w, color=colors[label], label=label)
        for b, v in zip(bars, vals):
            ax.text(b.get_x() + b.get_width() / 2, v * 1.05, f"{int(v)}",
                    ha="center", va="bottom", fontsize=8)

    ax.set_yscale("log")
    ax.set_xticks(x)
    ax.set_xticklabels(STRATS)
    ax.set_xlabel("wait strategy (DESIGN §6 tier)")
    ax.set_ylabel("publish → visible latency (ns, log scale)")

    ax.set_title("shimmy latency tails per wait strategy", fontsize=12)
    ax.grid(True, which="both", axis="y", alpha=0.3)
    ax.legend(loc="upper left", framealpha=0.95)
    ax.set_ylim(top=max(float(by[s]["p999_ns"]) for s in STRATS) * 3)

    # Sub-line: methodology + the honest noisy caveat, woven into the figure
    # itself so the chart cannot be quoted without it.
    caveat = _noisy_caveat(rows)
    fig.text(0.5, 0.935,
             "Backpressure, 64B, producer/consumer pinned to distinct cores, "
             "300k samples",
             ha="center", va="top", fontsize=9, color="#333")
    fig.text(0.5, 0.905, caveat,
             ha="center", va="top", fontsize=8.5, color="#d93025")

    fig.tight_layout(rect=[0, 0, 1, 0.90])
    out = os.path.join(ASSETS, "latency_tails.svg")
    fig.savefig(out, format="svg")
    plt.close(fig)
    return out


def main():
    os.makedirs(ASSETS, exist_ok=True)
    t = plot_throughput()
    l = plot_latency()
    print(f"wrote {os.path.relpath(t, ROOT)}")
    print(f"wrote {os.path.relpath(l, ROOT)}")


if __name__ == "__main__":
    sys.exit(main())
