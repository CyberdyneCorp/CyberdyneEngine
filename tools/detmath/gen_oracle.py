#!/usr/bin/env python3
"""High-precision references for the deterministic math kernel's property tests.

`openspec/changes/add-deterministic-math` task 2.3 and design §10.3. `integration.detmath_vectors`
asserts each function's DECLARED error bound (design §5.3) against these references and prints the
worst error it found beside the bound.

THE ORACLE SHARES NOTHING WITH THE KERNEL. It is mpmath at 50 significant digits, evaluating the real
function of the QUANTISED input: a function cannot be more accurate than its input, so `sin` of an
`Angle` is compared with the sine of exactly that binary angle, and `exp2` of a `Fixed` with two to
exactly that Q32.32 value. It does not import tools/detmath/model.py, so an error in the rules
cannot hide by being made twice.

THE FILE FORMAT, one function per file under tools/detmath/oracle/, one case per line:

    <input> [<input>] <reference>

Inputs are raw two's-complement bit patterns in hexadecimal (16 digits; an `Angle` is a `u32` in the
low half). The reference is the real value scaled by 2^64 and rounded to the nearest integer,
written as a signed hexadecimal number, so the C++ test compares integers and needs no
arbitrary-precision library:

    a Fixed result      value * 2^64           (the result's raw value is this divided by 2^32)
    an Angle result     turns * 2^64 mod 2^64  (the result's raw value is this divided by 2^32)
    `max`, `min`        the real value is beyond Fixed's range, or the input outside the function's
                        domain, and the declared result is that saturated value

USAGE

    python3 tools/detmath/gen_oracle.py            rewrite tools/detmath/oracle/
    python3 tools/detmath/gen_oracle.py --check    fail, naming the file, when one is stale
"""

from __future__ import annotations

import argparse
import pathlib
import sys

import mpmath
from mpmath import mp, mpf

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from model import I64_MAX, I64_MIN, ONE, SplitMix64, positive, ranged, scaled, wrap64  # noqa: E402

mp.dps = 50
HERE = pathlib.Path(__file__).resolve().parent
OUTPUT = HERE / "oracle"
SEED = 0x0AC1E_5EED
COUNT = 2048
TWO64 = mpf(2) ** 64
FIXED_MAX = mpf(I64_MAX) / 2 ** 32


def fixed(raw: int):
    return mpf(raw) / 2 ** 32


def turns(raw: int):
    return mpf(raw) / 2 ** 32


def fixed_reference(value) -> str:
    if value > FIXED_MAX:
        return "max"
    return _signed_hex(int(mpmath.nint(value * TWO64)))


def angle_reference(radians) -> str:
    """Radians in (-pi, pi], as turns * 2^64 reduced modulo one turn."""
    scaled_turns = int(mpmath.nint(radians / (2 * mp.pi) * TWO64))
    return _signed_hex(scaled_turns % (1 << 64))


def _signed_hex(value: int) -> str:
    return f"-{-value:x}" if value < 0 else f"{value:x}"


def _raw(value: int) -> str:
    return f"{value & ((1 << 64) - 1):016x}"


def clamp_unit(raw: int) -> int:
    return max(-ONE, min(ONE, raw))


#: name -> (arity, reference, edge inputs, seeded draw)
FUNCTIONS = {
    "sqrt": (1, lambda x: fixed_reference(mp.sqrt(fixed(x))) if x >= 0 else "0",
             [0, 1, 2, 3, 4, ONE, ONE - 1, ONE + 1, 2 * ONE, 4 * ONE, I64_MAX, I64_MAX - 1, 1 << 62,
              (1 << 62) - 1, 0xFFFFFFFF, -1, I64_MIN],
             lambda rng: (scaled(rng),)),
    "sin": (1, lambda a: fixed_reference(mp.sin(2 * mp.pi * turns(a))),
            [0, 1, 1 << 29, (1 << 29) - 1, (1 << 29) + 1, 1 << 30, 1 << 31, 3 << 30, 0xFFFFFFFF,
             5 << 29, 7 << 29],
            lambda rng: (rng.next() >> 32,)),
    "cos": (1, lambda a: fixed_reference(mp.cos(2 * mp.pi * turns(a))),
            [0, 1, 1 << 29, (1 << 29) - 1, (1 << 29) + 1, 1 << 30, 1 << 31, 3 << 30, 0xFFFFFFFF,
             3 << 29, 7 << 29],
            lambda rng: (rng.next() >> 32,)),
    "tan": (1, lambda a: _tan_reference(a),
            [0, 1, 1 << 29, (1 << 29) - 1, (1 << 30) - 1, (1 << 30) - 2, (1 << 30) - 16,
             (1 << 30) - (1 << 10), (1 << 30) + 1, (1 << 30) + 16, 0xFFFFFFFF, 3 << 29],
            lambda rng: (rng.next() >> 32,)),
    "atan": (1, lambda x: angle_reference(mp.atan(fixed(x))),
             [0, 1, -1, ONE, -ONE, I64_MAX, I64_MIN, 1 << 20, -(1 << 20), 1779033704, 1779033703],
             lambda rng: (scaled(rng),)),
    "atan2": (2, lambda y, x: angle_reference(mp.atan2(fixed(y), fixed(x))) if (x or y) else "0",
              [(0, 0), (0, ONE), (ONE, 0), (0, -ONE), (-ONE, 0), (ONE, ONE), (-ONE, -ONE),
               (ONE, -ONE), (-ONE, ONE), (I64_MAX, 1), (1, I64_MAX), (I64_MIN, I64_MIN),
               (I64_MIN, 1), (1, I64_MIN), (0, -1), (-1, 0), (3, 4), (-3, 4)],
              lambda rng: (scaled(rng), scaled(rng))),
    "asin": (1, lambda x: angle_reference(mp.asin(fixed(clamp_unit(x)))),
             [0, ONE, -ONE, ONE - 1, -ONE + 1, ONE + 1, -ONE - 1, ONE // 2, -ONE // 2,
              ONE - (1 << 16), I64_MAX, I64_MIN],
             lambda rng: (ranged(rng, -(5 << 30), 5 << 30),)),
    "acos": (1, lambda x: angle_reference(mp.acos(fixed(clamp_unit(x)))),
             [0, ONE, -ONE, ONE - 1, -ONE + 1, ONE + 1, -ONE - 1, ONE // 2, -ONE // 2,
              ONE - (1 << 16), I64_MAX, I64_MIN],
             lambda rng: (ranged(rng, -(5 << 30), 5 << 30),)),
    "exp2": (1, lambda x: fixed_reference(mpf(2) ** fixed(x)),
             [0, ONE, -ONE, 30 * ONE, 31 * ONE, 31 * ONE - 1, -32 * ONE, -33 * ONE, -34 * ONE,
              -64 * ONE, 1, -1, I64_MAX, I64_MIN],
             lambda rng: (ranged(rng, -(40 << 32), 40 << 32),)),
    "log2": (1, lambda x: fixed_reference(mp.log(fixed(x), 2)) if x > 0 else "min",
             [1, 2, ONE, ONE - 1, ONE + 1, 2 * ONE, I64_MAX, 3, 0x16A09E667, 0x16A09E668],
             lambda rng: (positive(rng),)),
    "exp": (1, lambda x: fixed_reference(mp.exp(fixed(x))),
            [0, ONE, -ONE, 21 * ONE, 22 * ONE, -22 * ONE, -23 * ONE, 1, -1],
            lambda rng: (ranged(rng, -(40 << 32), 40 << 32),)),
    "log": (1, lambda x: fixed_reference(mp.log(fixed(x))) if x > 0 else "min",
            [1, 2, ONE, ONE - 1, ONE + 1, 11674931555, I64_MAX],
            lambda rng: (positive(rng),)),
    "pow": (2, lambda x, y: fixed_reference(fixed(x) ** fixed(y)) if x > 0 else "0",
            [(ONE, 0), (ONE, ONE), (2 * ONE, 3 * ONE), (2 * ONE, -ONE), (4 * ONE, ONE // 2),
             (I64_MAX, 1), (1, ONE), (ONE + 1, 8 * ONE)],
            lambda rng: (positive(rng), ranged(rng, -(8 << 32), 8 << 32))),
}


def _tan_reference(angle: int) -> str:
    if angle == 1 << 30:
        return "max"  # the pole: cos is exactly zero and the kernel saturates
    if angle == 3 << 30:
        return "min"  # the other pole, from below: saturates to Fixed::min()
    value = mp.tan(2 * mp.pi * turns(angle))
    if value < mpf(I64_MIN) / 2 ** 32:
        return "min"
    return fixed_reference(value)


def render(name: str) -> str:
    arity, reference, edges, draw = FUNCTIONS[name]
    names = list(FUNCTIONS)
    rng = SplitMix64(SEED + 0x100 * (names.index(name) + 1))
    cases = [edge if isinstance(edge, tuple) else (edge,) for edge in edges]
    cases += [draw(rng) for _ in range(COUNT)]
    lines = [
        f"# detmath oracle: {name}. Generated by tools/detmath/gen_oracle.py; do not edit.",
        "# Real value of the quantised input, times 2^64, rounded; see the generator for the format.",
    ]
    for case in cases:
        if len(case) != arity:
            raise SystemExit(f"{name}: a case has {len(case)} inputs, the function takes {arity}")
        inputs = [wrap64(value) if value < (1 << 63) else value for value in case]
        lines.append(" ".join(_raw(value) for value in inputs) + " " + reference(*inputs))
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail when a file is stale")
    parser.add_argument("--output", type=pathlib.Path, default=OUTPUT)
    arguments = parser.parse_args()

    stale = []
    for name in FUNCTIONS:
        text = render(name)
        path = arguments.output / f"{name}.txt"
        if arguments.check:
            if not path.exists() or path.read_text(encoding="utf-8") != text:
                stale.append(str(path))
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    if stale:
        print("stale oracle files (run python3 tools/detmath/gen_oracle.py):", *stale, sep="\n  ",
              file=sys.stderr)
        return 1
    print("oracle: current" if arguments.check else f"wrote {len(FUNCTIONS)} files to {arguments.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
