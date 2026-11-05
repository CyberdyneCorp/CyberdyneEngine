#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draw the session measurement from `cy_net_session_bench`'s own output. Issue #82.

    build/<profile>/src/networking/tools/cy_net_session_bench > session-bench.csv
    python3 src/networking/tools/session_curve.py session-bench.csv net-session-bandwidth.png

Two panels against the world's population: what one client receives per second on the wire, beside
the most its budget allows; and what one server tick costs. Every point is a number the engine
produced in the run whose CSV is passed in: ENGINE OUTPUT, not a diagram, and the figure says so.
"""

import csv
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def read(path):
    with open(path, newline="", encoding="utf-8") as handle:
        return [{key: int(value) for key, value in row.items()} for row in csv.DictReader(handle)]


def draw(rows, out):
    entities = [row["entities"] for row in rows]
    wire = [row["wire_bytes_per_s"] / 1024 for row in rows]
    budget = [row["budget"] * row["tick_rate"] / 1024 for row in rows]
    mean = [row["server_us_mean"] for row in rows]
    p95 = [row["server_us_p95"] for row in rows]

    figure, (top, bottom) = plt.subplots(2, 1, figsize=(9, 7.5), sharex=True)
    figure.suptitle(
        "CyberNet: a dedicated server and two clients against entity count\n"
        "engine output, not a diagram",
        fontsize=12,
        fontweight="bold",
    )
    top.plot(entities, wire, marker="o", label="wire bytes per second to one client")
    top.plot(entities, budget, linestyle="--", color="grey",
             label="replication payload budget x tick rate (4096 B x 30 Hz);\n"
             "the wire adds headers, tags, fragments, pings and acks")
    top.set_ylabel("KiB per second")
    top.set_title("200 ms round trip, 5 % loss, 5 % reordering; every unit moving every tick")
    top.legend(loc="lower right")
    top.grid(alpha=0.3)
    bottom.plot(entities, mean, marker="o", label="mean")
    bottom.plot(entities, p95, marker="s", label="95th percentile")
    bottom.set_xscale("log")
    bottom.set_xlabel("replicated entities")
    bottom.set_ylabel("microseconds per server tick")
    bottom.set_title("one server update that ran a tick: transport, simulation, gather, two snapshots")
    bottom.legend(loc="upper left")
    bottom.grid(alpha=0.3)
    figure.tight_layout()
    figure.savefig(out, dpi=100)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: session_curve.py <session-bench.csv> <out.png>")
    draw(read(sys.argv[1]), sys.argv[2])
