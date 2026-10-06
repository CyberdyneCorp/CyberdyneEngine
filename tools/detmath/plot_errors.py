#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draw docs/design/images/detmath-errors.png: the kernel's measured errors against its bounds.

Left, each function's worst error over the committed oracle (tools/detmath/oracle/) as a fraction of
its declared bound. Right, the error of `sin` over a quarter turn, in ulps. The outputs are the
model's (tools/detmath/model.py); integration.detmath_vectors holds the C++ to the same bits through
the golden vectors, so the picture is of the engine's arithmetic, not of an approximation of it.

    python3 tools/detmath/plot_errors.py [--output PATH]

Needs matplotlib and mpmath. Not part of any gate: the bounds themselves are asserted by the suite.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from mpmath import mp  # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import model  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent
OUTPUT = model.REPO / "docs/design/images/detmath-errors.png"
ANGLE_OUTPUT = {"atan", "atan2", "asin", "acos"}
ULP = 1 << 32

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
SERIES = "#2a78d6"


def allowed(name: str, inputs: list[int], reference: int) -> float:
    """The declared bound of functions.h, in units of 2^-64, as integration.detmath_vectors has it."""
    magnitude = abs(reference)
    if name == "sqrt":
        return 2 ** 31
    if name in ("sin", "cos", "atan", "atan2"):
        return ULP
    if name == "tan":
        return magnitude >> 30 if magnitude > 1 << 64 else 2 * ULP
    if name in ("asin", "acos"):
        edge = ONE - (1 << 16)
        return 2 * ULP if -edge <= inputs[0] <= edge else 2 ** 49
    if name == "exp2":
        return max(ULP, magnitude >> 36)
    if name == "log2":
        return 2 * ULP
    if name == "exp":
        return max(2 * ULP, magnitude >> 34)
    if name == "log":
        return 3 * ULP
    if name == "pow":
        return max(2 * ULP, (magnitude >> 34) + (((magnitude >> 58) * abs(inputs[1])) >> 32))
    raise KeyError(name)


ONE = model.ONE


def worst_fractions(kernel: model.Kernel) -> list[tuple[str, float]]:
    rows = []
    for name in ("sqrt", "sin", "cos", "tan", "atan", "atan2", "asin", "acos", "exp2", "log2",
                 "exp", "log", "pow"):
        worst = 0.0
        for line in (HERE / "oracle" / f"{name}.txt").read_text(encoding="utf-8").splitlines():
            if line.startswith("#"):
                continue
            *raw_inputs, reference_text = line.split()
            if reference_text in ("max", "min"):
                continue
            inputs = [model.wrap64(int(text, 16)) for text in raw_inputs]
            reference = int(reference_text, 16)
            result = getattr(kernel, name)(*inputs)
            if name in ANGLE_OUTPUT:
                difference = ((result << 32) - reference) % (1 << 64)
                difference -= (1 << 64) if difference >> 63 else 0
            else:
                difference = (result << 32) - reference
            worst = max(worst, abs(difference) / allowed(name, inputs, reference))
        rows.append((name, worst))
    return rows


def sine_errors(kernel: model.Kernel, count: int = 2048) -> tuple[list[float], list[float]]:
    mp.dps = 40
    turns, errors = [], []
    for index in range(count + 1):
        angle = (index * (1 << 30)) // count + (index * 7919) % 97  # off the round grid
        angle = min(angle, 1 << 30)
        exact = mp.sin(2 * mp.pi * mp.mpf(angle) / (1 << 32)) * ULP
        turns.append(angle / float(1 << 32))
        errors.append(float(kernel.sin(angle) - exact))
    return turns, errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output", type=pathlib.Path, default=OUTPUT)
    arguments = parser.parse_args()

    kernel = model.Kernel(model.load_coefficients())
    rows = worst_fractions(kernel)
    turns, errors = sine_errors(kernel)

    plt.rcParams.update({"font.size": 10, "text.color": INK, "axes.labelcolor": INK_SECONDARY,
                         "xtick.color": INK_SECONDARY, "ytick.color": INK_SECONDARY})
    figure, (bars, line) = plt.subplots(1, 2, figsize=(11, 4.4), dpi=150,
                                        gridspec_kw={"width_ratios": [1, 1.25]})
    figure.patch.set_facecolor(SURFACE)
    for axis in (bars, line):
        axis.set_facecolor(SURFACE)
        for side in ("top", "right"):
            axis.spines[side].set_visible(False)
        for side in ("left", "bottom"):
            axis.spines[side].set_color("#d6d5d0")

    names = [name for name, _ in rows][::-1]
    values = [100.0 * value for _, value in rows][::-1]
    bars.barh(names, values, color=SERIES, height=0.6)
    bars.set_xlim(0, 110)
    bars.axvline(100, color=INK_SECONDARY, linewidth=1, linestyle="--")
    bars.text(100, len(names) - 0.4, " declared bound", color=INK_SECONDARY, va="bottom", fontsize=9)
    bars.set_xlabel("worst error over the oracle, % of the declared bound")
    bars.set_title("Every function inside its bound (sqrt is exact rounding)", loc="left", color=INK,
                   fontsize=11)
    for position, value in enumerate(values):
        bars.text(value + 1.5, position, f"{value:.0f}%", va="center", fontsize=8, color=INK_SECONDARY)
    bars.grid(axis="x", color="#ecebe7", linewidth=0.8)
    bars.set_axisbelow(True)

    line.scatter(turns, errors, color=SERIES, s=3, linewidths=0)
    line.axhline(0.5, color=INK_SECONDARY, linewidth=1, linestyle="--")
    line.axhline(-0.5, color=INK_SECONDARY, linewidth=1, linestyle="--")
    line.set_ylim(-1.1, 1.1)
    line.set_xlim(0, 0.25)
    line.set_xlabel("angle, turns")
    line.set_ylabel("sin error, ulp (2^-32)")
    line.set_title("sin over a quarter turn: within the rounding's ±0.5 ulp (bound 1)", loc="left",
                   color=INK, fontsize=11)
    line.text(0.251, 0.5, " +0.5", va="center", fontsize=8, color=INK_SECONDARY, clip_on=False)
    line.grid(axis="y", color="#ecebe7", linewidth=0.8)

    figure.tight_layout()
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(arguments.output, facecolor=SURFACE)
    print(f"wrote {arguments.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
