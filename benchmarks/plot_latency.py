#!/usr/bin/env python3
"""Spec 009 T5: render a latency-distribution CSV (velox_loadgen's --csv-out) into a PNG.

Log-x percentile curve, corrected vs naive as two series, horizontal budget lines at
2/20/100 us. Zero dependencies beyond matplotlib -- and if matplotlib is missing, this exits 0
with a clear message rather than breaking /bench (a missing optional plotter must never fail
the build).

Usage: plot_latency.py <path/to/latency-<scenario>.csv> [hardware-string]
"""

import csv
import sys


def main():
    if len(sys.argv) < 2:
        print("usage: plot_latency.py <csv> [hardware-string]", file=sys.stderr)
        return 2

    csv_path = sys.argv[1]
    hardware = sys.argv[2] if len(sys.argv) > 2 else "hardware not specified"

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed -- skipping plot generation (this is not a failure; "
              "the CSV at", csv_path, "is the durable artifact).")
        return 0

    percentiles, corrected, naive = [], [], []
    with open(csv_path) as f:
        reader = csv.reader(f)
        header = next(reader)
        for row in reader:
            percentiles.append(float(row[0]))
            corrected.append(float(row[1]))
            naive.append(float(row[2]))

    # log-x needs the "distance from 100" for percentiles, not the raw value (100 -> infinity).
    xs = [max(100.0 - p, 0.001) for p in percentiles]

    fig, ax = plt.subplots(figsize=(9, 6))
    ax.plot(xs, corrected, marker="o", label="corrected (report this)", color="#2b6cb0")
    ax.plot(xs, naive, marker="s", label="naive (what an uncorrected loop reports)",
           color="#c53030", linestyle="--")

    for budget_ns, label in [(2000, "p50 budget 2us"), (20000, "p99 budget 20us"),
                             (100000, "p999 budget 100us")]:
        ax.axhline(y=budget_ns, color="gray", linestyle=":", linewidth=1)
        ax.text(xs[0], budget_ns, label, fontsize=8, va="bottom", color="gray")

    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.invert_xaxis()
    ax.set_xlabel("percentile (log scale, closer to 100 on the right)")
    ax.set_ylabel("latency (ns, log scale)")
    ax.set_title("velox order-to-match latency distribution\n" + hardware, fontsize=10)
    ax.legend()
    ax.grid(True, which="both", alpha=0.3)

    out_path = csv_path.rsplit(".", 1)[0] + ".png"
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
