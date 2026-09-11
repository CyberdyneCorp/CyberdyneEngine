#!/usr/bin/env python3
"""Render docs/design/images/m8c-ml-inference.png from `integration.ml_onnxruntime`'s own output.

THIS IS A PLOT OF THE ENGINE'S OWN MEASURED OUTPUT, NOT A DIAGRAM, and the distinction is the one
M8.c's brief insists on. Every number on the figure is parsed from a run of the test binary — the
class scores ONNX Runtime returned for three agents, the per-invocation cost it measured, and the
frame the scheduler reported. Nothing is drawn from a table this script owns; if the suite has not
been run, this script exits non-zero and draws nothing.

    build/<dir>/cy_test_integration_ml_onnxruntime --success > run.txt
    python3 src/ml/tools/plot_capture.py run.txt

The panel labels say where each number came from, because a reader who cannot tell a measurement
from a decoration cannot judge either.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402  (after the backend is fixed)

OUTPUT = Path(__file__).resolve().parents[3] / "docs" / "design" / "images" / "m8c-ml-inference.png"

CLASSES = ("ignore", "engage", "retreat")
ROWS = (
    "close, hidden,\nhostile, armed",
    "close, exposed,\nhostile, dry",
    "far, calm,\nunknown, dry",
)

INK = "#101418"
MUTED = "#5d6773"
GRID = "#d8dee6"
SERIES = ("#4c78a8", "#f58518", "#54a24b")


def parse(text: str) -> dict:
    """Pull the measurements out of the suite's stdout. Missing ones are an error."""
    found: dict = {}

    scores = re.search(
        r"\[ml\] scores\s+([-\d.eE ,]+)\n\[ml\] scores\s+([-\d.eE ,]+)\n\[ml\] scores\s+([-\d.eE ,]+)",
        text,
    )
    if scores:
        found["scores"] = [
            [float(value) for value in row.replace(",", " ").split()] for row in scores.groups()
        ]

    mean = re.search(r"\[ml\] onnxruntime mean (\d+) ns over (\d+) invocations", text)
    if mean:
        found["mean_ns"] = int(mean.group(1))
        found["invocations"] = int(mean.group(2))

    frame = re.search(r"\[ml\] frame spent (\d+) ns, utilisation ([\d.]+)", text)
    if frame:
        found["frame_ns"] = int(frame.group(1))
        found["utilisation"] = float(frame.group(2))

    budget = re.search(r"\[ml\] budget (\d+) submitted (\d+) dispatched (\d+) deferred", text)
    if budget:
        found["submitted"] = int(budget.group(1))
        found["dispatched"] = int(budget.group(2))
        found["deferred"] = int(budget.group(3))

    zero = re.search(r"\[ml\] zero-input\s+([-\d.eE ,]+)", text)
    if zero:
        found["zero"] = [float(value) for value in zero.group(1).replace(",", " ").split()]

    passed = re.search(r"assertions:\s+(\d+) \|\s+(\d+) passed", text)
    if passed:
        found["assertions"] = int(passed.group(1))
        found["assertions_passed"] = int(passed.group(2))

    return found


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <path to the suite's stdout>", file=sys.stderr)
        return 2
    source = Path(sys.argv[1])
    if not source.exists():
        print(f"{source}: no such file. Run the suite first.", file=sys.stderr)
        return 2

    measured = parse(source.read_text())
    required = ("scores", "mean_ns", "frame_ns", "submitted", "zero", "assertions")
    missing = [key for key in required if key not in measured]
    if missing:
        # RULE: an artefact that reports a gap does not exit zero. A figure drawn from half a run
        # would be a figure a reader could not tell from a full one.
        print(f"{source}: no measurement for {', '.join(missing)}", file=sys.stderr)
        return 1

    figure = plt.figure(figsize=(11.5, 5.4), dpi=170)
    figure.patch.set_facecolor("white")
    figure.suptitle(
        "CyberML through ONNX Runtime — every number below is this suite's own output",
        fontsize=13,
        color=INK,
        y=0.97,
    )
    figure.text(
        0.5,
        0.905,
        f"integration.ml_onnxruntime, {measured['assertions_passed']} of "
        f"{measured['assertions']} assertions passed · "
        "src/ml/assets/threat_classifier.onnx, 637 bytes, committed",
        ha="center",
        fontsize=9,
        color=MUTED,
    )

    grid = figure.add_gridspec(1, 2, width_ratios=(1.55, 1.0), wspace=0.28,
                               left=0.07, right=0.97, top=0.80, bottom=0.13)

    # --- Left: what the model answered, per agent, in one batched call -------------------------
    left = figure.add_subplot(grid[0, 0])
    width = 0.26
    positions = range(len(ROWS))
    for index, name in enumerate(CLASSES):
        values = [row[index] for row in measured["scores"]]
        offsets = [position + (index - 1) * width for position in positions]
        bars = left.bar(offsets, values, width, label=name, color=SERIES[index], zorder=3)
        for bar, value in zip(bars, values):
            left.text(
                bar.get_x() + bar.get_width() / 2,
                value + 0.02,
                f"{value:.3f}",
                ha="center",
                fontsize=7.5,
                color=MUTED,
            )
    left.set_xticks(list(positions))
    left.set_xticklabels(ROWS, fontsize=8.5, color=INK)
    left.set_ylim(0, 1.12)
    left.set_ylabel("softmax score", fontsize=9, color=MUTED)
    left.set_title(
        "three agents, one batched session call — argmax differs per row",
        fontsize=10,
        color=INK,
        pad=8,
    )
    left.legend(frameon=False, fontsize=8.5, ncol=3, loc="upper center")
    left.grid(axis="y", color=GRID, linewidth=0.7, zorder=0)
    left.set_axisbelow(True)
    for spine in ("top", "right"):
        left.spines[spine].set_visible(False)
    for spine in ("left", "bottom"):
        left.spines[spine].set_color(GRID)

    # --- Right: the measurements that are not scores -------------------------------------------
    right = figure.add_subplot(grid[0, 1])
    right.axis("off")
    zero = ", ".join(f"{value:.6f}" for value in measured["zero"])
    lines = [
        ("all-zero input", zero),
        ("", "the output bias through softmax, computable on paper"),
        ("", ""),
        ("cost per invocation", f"{measured['mean_ns'] / 1000.0:,.1f} µs"),
        ("", f"mean over {measured['invocations']} runs, allocating twice in total"),
        ("", ""),
        ("frame budget", f"{measured['submitted']} submitted → "
                         f"{measured['dispatched']} dispatched, {measured['deferred']} deferred"),
        ("", f"{measured['frame_ns'] / 1000.0:,.1f} µs spent, "
             f"utilisation {measured['utilisation']:.2f}"),
    ]
    y = 0.92
    for label, value in lines:
        if label:
            right.text(0.0, y, label, fontsize=9, color=MUTED, va="top")
            right.text(0.0, y - 0.075, value, fontsize=11, color=INK, va="top", family="monospace")
            y -= 0.185
        elif value:
            right.text(0.0, y, value, fontsize=8, color=MUTED, va="top", style="italic")
            y -= 0.075
        else:
            y -= 0.03
    right.set_title("what the run measured", fontsize=10, color=INK, pad=8, loc="left")

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(OUTPUT, facecolor="white")
    print(f"wrote {OUTPUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
