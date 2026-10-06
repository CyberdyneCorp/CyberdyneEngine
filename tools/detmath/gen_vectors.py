#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write the deterministic math kernel's golden vectors and its committed digests.

`openspec/changes/add-deterministic-math` tasks 1.5 and 2.4, design §10.1 and §10.2.

WHAT IS WRITTEN, under tools/detmath/vectors/

    <function>.txt   the edge cases (zero, ±1 ulp, the ends of the range, octant boundaries, the axes
                     of atan2, the overflow boundaries, the rounding ties) and the first 256 inputs of
                     the seeded sweep, each with its expected raw output
    digests.txt      the kernel version, the sweep size, every function's digest over the full
                     16 384-input sweep, and the kernel digest that folds them

The expected outputs come from tools/detmath/model.py: the rules of design §4.3 written a second
time in Python integers. `integration.detmath_vectors` checks every line and every digest on each
CI leg ALONE, so a leg whose kernel diverges fails by itself, before any comparison between legs.

REGENERATION IS LOUD, ON BOTH SIDES. This script rewrites the files from the model;
`CY_DETMATH_RECORD_GOLDEN=1` makes the C++ suite rewrite the outputs it computed AND FAIL THE RUN,
so new expectations can never be accepted inside a green job. Either way the diff is reviewed, and a
moved output means `detmath::kKernelVersion` moves with it.

USAGE

    python3 tools/detmath/gen_vectors.py            rewrite tools/detmath/vectors/
    python3 tools/detmath/gen_vectors.py --check    fail, naming the file, when one is stale
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import model  # noqa: E402
from model import I64_MAX, I64_MIN, ONE  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent
OUTPUT = HERE / "vectors"
VERSION_HEADER = model.REPO / "src/core/detmath/include/cy/core/detmath/version.h"

QUARTER = 1 << 30
HALF = 1 << 31
EIGHTH = 1 << 29
PI = 13493037705
SQRT2_TOP = 0x16A09E667  # just below sqrt(2) as a raw value: the log2 reduction's boundary

#: Inputs a seeded sweep would almost never draw, and which are exactly where a kernel goes wrong.
EDGES: dict[str, list[tuple[int, ...]]] = {
    "add": [(0, 0), (I64_MAX, 1), (I64_MIN, -1), (I64_MAX, I64_MAX), (I64_MIN, I64_MIN), (-1, 1),
            (ONE, ONE), (I64_MAX, I64_MIN)],
    "sub": [(0, 0), (I64_MIN, 1), (I64_MAX, -1), (0, I64_MIN), (I64_MIN, I64_MIN), (ONE, -ONE),
            (-1, I64_MAX)],
    "mul": [(0, I64_MAX), (ONE, ONE), (ONE, -ONE), (I64_MAX, I64_MAX), (I64_MIN, I64_MIN),
            (I64_MIN, -ONE), (ONE, I64_MIN), (1, 1), (-1, 1), (I64_MAX, 1), (I64_MIN, 1),
            # Rounding ties: the product is exactly halfway between two raw values. Ties go toward
            # +infinity, so 0.5 -> 1, 2.5 -> 3, -0.5 -> 0, -2.5 -> -2 (ulp units).
            (1, 1 << 31), (5, 1 << 31), (-1, 1 << 31), (-5, 1 << 31), (3, 3 << 31), (-3, 3 << 31),
            (1 << 31, 1 << 31), (3 << 16, 1 << 47), (-(3 << 16), 1 << 47)],
    "div": [(0, 0), (ONE, 0), (-ONE, 0), (I64_MIN, 0), (ONE, 3 * ONE), (-ONE, 3 * ONE),
            (ONE, -3 * ONE), (I64_MIN, -1), (I64_MIN, -ONE), (I64_MAX, 1), (1, I64_MAX),
            (I64_MAX, I64_MAX), (I64_MIN, I64_MIN), (7, 2), (-7, 2), (ONE, 1), (-ONE, 1)],
    "sqrt": [(0,), (1,), (2,), (ONE,), (4 * ONE,), (2 * ONE,), (I64_MAX,), (-1,), (I64_MIN,),
             (ONE - 1,), (ONE + 1,), ((1 << 62) - 1,)],
    "sqrt_wide": [(0, 0), (0, 1), (1, 0), (-1, (1 << 64) - 1), (I64_MAX, (1 << 64) - 1),
                  ((1 << 62) - 1, (1 << 64) - 1), (1 << 62, 0), (0, (1 << 64) - 1),
                  (I64_MIN, 0)],
    "narrow16": [(0,), (1 << 15,), (-(1 << 15),), ((1 << 15) - 1,), (3 << 15,), (-(3 << 15),),
                 (I64_MAX,), (I64_MIN,), ((1 << 47) - (1 << 15),), (-(1 << 47) - (1 << 15) - 1,),
                 (-(1 << 47) - (1 << 15),)],
    "angle_scale": [(QUARTER, 2 * ONE), (QUARTER, -ONE), (0xFFFFFFFF, ONE), (0xFFFFFFFF, -ONE),
                    (1, 1 << 31), (3, 1 << 31), (HALF, 3 * ONE), (0, I64_MAX >> 16)],
    "angle_from_radians": [(0,), (PI,), (-PI,), (2 * PI,), (PI // 2,), (I64_MAX,), (I64_MIN,),
                           (1,), (-1,)],
    "angle_radians": [(0,), (QUARTER,), (HALF,), (3 * QUARTER,), (0xFFFFFFFF,), (1,)],
    "sin": [(0,), (1,), (EIGHTH,), (EIGHTH - 1,), (EIGHTH + 1,), (QUARTER,), (HALF,),
            (3 * QUARTER,), (0xFFFFFFFF,)] + [(k * EIGHTH,) for k in range(8)],
    "cos": [(0,), (1,), (EIGHTH,), (QUARTER,), (QUARTER - 1,), (HALF,), (3 * QUARTER,),
            (0xFFFFFFFF,)] + [(k * EIGHTH,) for k in range(8)],
    "tan": [(0,), (1,), (EIGHTH,), (QUARTER,), (QUARTER - 1,), (QUARTER + 1,), (3 * QUARTER,),
            (3 * QUARTER - 1,), (HALF,), (0xFFFFFFFF,)],
    "atan": [(0,), (ONE,), (-ONE,), (I64_MAX,), (I64_MIN,), (1,), (-1,), (1779033704,)],
    "atan2": [(0, 0), (0, ONE), (ONE, 0), (0, -ONE), (-ONE, 0), (ONE, ONE), (-ONE, ONE),
              (ONE, -ONE), (-ONE, -ONE), (I64_MIN, I64_MIN), (I64_MAX, I64_MIN), (I64_MIN, 0),
              (0, I64_MIN), (1, I64_MAX), (I64_MAX, 1), (3, 4), (-3, -4)],
    "asin": [(0,), (ONE,), (-ONE,), (ONE + 1,), (-ONE - 1,), (ONE // 2,), (I64_MAX,), (I64_MIN,),
             (ONE - 1,)],
    "acos": [(0,), (ONE,), (-ONE,), (ONE + 1,), (-ONE - 1,), (ONE // 2,), (I64_MAX,), (I64_MIN,),
             (ONE - 1,)],
    "exp2": [(0,), (ONE,), (-ONE,), (30 * ONE,), (31 * ONE - 1,), (31 * ONE,), (-32 * ONE,),
             (-33 * ONE,), (-34 * ONE,), (-94 * ONE,), (I64_MAX,), (I64_MIN,), (1,), (-1,)],
    "log2": [(1,), (2,), (ONE,), (ONE - 1,), (ONE + 1,), (I64_MAX,), (0,), (-1,), (I64_MIN,),
             (SQRT2_TOP,), (SQRT2_TOP + 1,)],
    "exp": [(0,), (ONE,), (-ONE,), (21 * ONE,), (22 * ONE,), (-23 * ONE,), (I64_MAX,), (I64_MIN,)],
    "log": [(1,), (ONE,), (I64_MAX,), (0,), (-ONE,), (11674931555,)],
    # The Q2.62 kernel values, at the ends of each reduced interval and at the reductions' seams.
    "sin_core": [(0,), (1,), (EIGHTH - 1,), (EIGHTH,), (QUARTER,), (0xFFFFFFFF,)],
    "cos_core": [(0,), (1,), (EIGHTH - 1,), (EIGHTH,), (QUARTER,), (0xFFFFFFFF,)],
    "atan_core": [(0,), (1,), (1910222894239003202,), (1910222894239003203,), ((1 << 62) - 1,),
                  (1 << 62,)],
    "exp2_core": [(0,), (1,), (1 << 61,), ((1 << 62) - 1,)],
    "log2_core": [(1,), (ONE,), (SQRT2_TOP,), (SQRT2_TOP + 1,), (I64_MAX,), (3,)],
    "pow": [(ONE, 0), (2 * ONE, 3 * ONE), (2 * ONE, -ONE), (4 * ONE, ONE // 2), (0, ONE),
            (-ONE, ONE), (I64_MAX, ONE), (I64_MAX, 2 * ONE), (1, ONE), (ONE + 1, I64_MAX)],
}


def kernel_version() -> int:
    match = re.search(r"kKernelVersion = (\d+);", VERSION_HEADER.read_text(encoding="utf-8"))
    if match is None:
        raise SystemExit(f"{VERSION_HEADER}: no kKernelVersion")
    return int(match.group(1))


def hex64(value: int) -> str:
    return f"{value & model.MASK64:016x}"


def render_function(kernel: model.Kernel, function: str, version: int) -> str:
    rng = model.SplitMix64(model.function_seed(model.FUNCTIONS.index(function)))
    lines = [
        f"# detmath golden vectors: {function}. Generated by tools/detmath/gen_vectors.py from",
        "# tools/detmath/model.py; do not edit. Raw two's-complement bit patterns in hexadecimal:",
        "# the inputs, then the expected output. `edge` lines are chosen; `seed` lines are the",
        "# first inputs of the sweep digests.txt folds.",
        f"# kernel-version {version}",
    ]
    for inputs in EDGES[function]:
        lines.append("edge " + " ".join(hex64(value) for value in inputs) + " "
                     + hex64(model.evaluate(kernel, function, inputs)))
    for _ in range(model.GOLDEN_SEEDED):
        inputs = model.draw(function, rng)
        lines.append("seed " + " ".join(hex64(value) for value in inputs) + " "
                     + hex64(model.evaluate(kernel, function, inputs)))
    return "\n".join(lines) + "\n"


def render_digests(kernel: model.Kernel, version: int) -> str:
    per_function = {function: model.function_digest(kernel, function) for function in model.FUNCTIONS}
    lines = [
        "# detmath kernel digests. Generated by tools/detmath/gen_vectors.py; do not edit.",
        "# Each function over its seeded sweep, folded in order, then every function folded into",
        "# `kernel`. See src/core/detmath/include/cy/core/detmath/digest.h.",
        f"kernel-version {version}",
        f"sweep-count {model.SWEEP_COUNT}",
    ]
    lines += [f"{function} {per_function[function]:016x}" for function in model.FUNCTIONS]
    lines.append(f"kernel {model.kernel_digest(per_function, version):016x}")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail when a file is stale")
    parser.add_argument("--output", type=pathlib.Path, default=OUTPUT)
    arguments = parser.parse_args()

    kernel = model.Kernel(model.load_coefficients())
    version = kernel_version()
    files = {f"{function}.txt": render_function(kernel, function, version)
             for function in model.FUNCTIONS}
    files["digests.txt"] = render_digests(kernel, version)

    stale = []
    for name, text in files.items():
        path = arguments.output / name
        if arguments.check:
            if not path.exists() or path.read_text(encoding="utf-8") != text:
                stale.append(str(path))
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    if stale:
        print("stale golden vectors (run python3 tools/detmath/gen_vectors.py, and bump "
              "detmath::kKernelVersion if an output moved):", *stale, sep="\n  ", file=sys.stderr)
        return 1
    print("vectors: current" if arguments.check else f"wrote {len(files)} files to {arguments.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
