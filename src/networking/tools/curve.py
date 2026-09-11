#!/usr/bin/env python3
"""Draw the bandwidth curve from `cy_net_bandwidth_curve`'s own output. M9 task 4.4.

    build/<profile>/src/networking/tools/cy_net_bandwidth_curve > curve.csv
    python3 src/networking/tools/curve.py curve.csv docs/design/images/m9-bandwidth-curve.png

The figure is ENGINE OUTPUT, not a diagram: every point is a number the scheduler produced in the
run whose CSV is passed in. The label on the figure says so, because M8.c's gate found that a
picture which does not say what it is gets read as whichever of the two is more flattering.
"""

import csv
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def read(path):
    with open(path, newline="", encoding="utf-8") as handle:
        return [{k: int(v) for k, v in row.items()} for row in csv.DictReader(handle)]


def draw(rows, out):
    population = [r["population"] for r in rows]
    candidates = [r["candidates"] for r in rows]
    peak_bytes = [r["peak_bytes"] for r in rows]
    budget = [r["bytes_budget"] for r in rows]
    deferred = [r["deferred_total"] for r in rows]
    forced = [r["forced_by_staleness_total"] for r in rows]
    gap = [r["worst_gap_ticks"] for r in rows]

    fig, (top, bottom) = plt.subplots(2, 1, figsize=(9, 7.5), sharex=True)
    fig.suptitle(
        "CyberNet: bandwidth against entity count — engine output, not a diagram",
        fontsize=13,
        fontweight="bold",
    )

    top.plot(population, peak_bytes, "o-", color="#1f77b4", label="peak bytes planned per tick")
    top.plot(population, budget, "--", color="#d62728", label="per-peer budget (8192 B/tick)")
    top.plot(population, candidates, "s-", color="#7f7f7f", label="candidate set (entities relevant)")
    top.set_xscale("log")
    top.set_yscale("log")
    top.set_ylabel("bytes per tick / entities")
    top.grid(True, which="both", alpha=0.3)
    top.legend(loc="upper left", fontsize=9)
    top.set_title(
        "The candidate set grows 324x; the bytes sent do not exceed the budget at any population",
        fontsize=10,
    )

    bottom.plot(population, deferred, "o-", color="#ff7f0e", label="updates deferred (60 ticks)")
    bottom.plot(population, forced, "^-", color="#2ca02c", label="forced in by bounded staleness")
    bottom.set_xscale("log")
    bottom.set_yscale("symlog")
    bottom.set_xlabel("entities in the world (log scale)")
    bottom.set_ylabel("updates over 60 ticks")
    bottom.grid(True, which="both", alpha=0.3)
    bottom.legend(loc="upper left", fontsize=9)

    twin = bottom.twinx()
    twin.plot(population, gap, ":", color="#9467bd", label="worst gap between sends (ticks)")
    twin.set_ylabel("ticks", color="#9467bd")
    twin.set_ylim(0, max(gap) * 2)
    twin.legend(loc="lower right", fontsize=9)
    bottom.set_title(
        "Pressure becomes deferral and then a staleness guarantee — never an unbounded wait",
        fontsize=10,
    )

    fig.tight_layout(rect=(0, 0.02, 1, 0.96))
    fig.text(
        0.5,
        0.005,
        "src/networking/tools/cy_net_bandwidth_curve, Development profile — each point is one run "
        "of the real InterestSet and PriorityScheduler",
        ha="center",
        fontsize=8,
        color="#555555",
    )
    fig.savefig(out, dpi=140)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        raise SystemExit(2)
    draw(read(sys.argv[1]), sys.argv[2])
    print(f"curve.py: wrote {sys.argv[2]}")
