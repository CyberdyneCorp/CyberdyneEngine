#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fit the deterministic math kernel's polynomials and write them as integer constants.

`openspec/changes/add-deterministic-math` task 2.1 and design §5. The coefficients ARE the definition
of `sin`, `atan`, `exp2` and `log2` in this engine: two peers agree because they compile the same
integers, and a reviewer reads a change to one of them as a change to the function. So they are
produced by this script, from a stated method, and committed — never typed.

THE METHOD, PER POLYNOMIAL

  1. Name the function on its reduced interval (design §5.2) in the variable the kernel evaluates
     in: `u = t*t` for the odd and even trigonometric halves, `f` for `exp2`, `s*s` for `log2`.
  2. Fit it with mpmath's Chebyshev interpolation (`mpmath.chebyfit`) at 60 significant digits,
     for each number of coefficients from 2 upwards.
  3. Round every coefficient to the kernel format, Q2.62 in an `i64`.
  4. Measure two errors over a dense grid of the reduced argument:
       fit        the polynomial with the ROUNDED coefficients, evaluated exactly in mpmath
       evaluated  the kernel's own integer evaluation (tools/detmath/model.py, the same rules the
                  C++ follows), against the exact function
  5. Keep the first degree whose FIT error is within the target below.

Both errors are written into the header beside the coefficients, because a coefficient table that
does not say how good it is invites somebody to "improve" it by hand.

THE TARGETS are set well below one unit of the output format (2^-32), so that the declared bounds of
design §5.3 are met by the final rounding rather than by the polynomial:

    sin, cos   2^-44 on [0, pi/4]                (output ulp 2^-32)
    atan       2^-44 turns on [0, tan(pi/8)]     (Angle ulp 2^-32 turn)
    exp2       2^-46 on [0, 1)                   (2^f lies in [1, 2), so absolute is relative)
    log2       2^-44 on |s| <= 3 - 2*sqrt(2)

USAGE

    python3 tools/detmath/gen_coefficients.py            rewrite the header
    python3 tools/detmath/gen_coefficients.py --check    fail, naming the drift, when it is stale

`just generate-check` runs `--check`, and runs the generator twice into scratch files to show that
the output is a function of this script alone. mpmath is pinned in CI (`mpmath==1.3.0`): a different
release may place the Chebyshev nodes differently and move a last bit, which `--check` would report
as drift rather than hide.
"""

from __future__ import annotations

import argparse
import difflib
import pathlib
import sys
import textwrap

import mpmath
from mpmath import mp, mpf

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import model  # noqa: E402

mp.dps = 60
SCALE = mpf(2) ** 62
GRID = 2048


def quantize(values: list) -> list[int]:
    return [int(mpmath.nint(value * SCALE)) for value in values]


def exact_poly(coefficients: list[int], x) -> mpf:
    """The rounded polynomial, low order first, evaluated with no rounding at all."""
    total = mpf(0)
    for coefficient in reversed(coefficients):
        total = total * x + mpf(coefficient) / SCALE
    return total


def log2_of(value) -> str:
    if value == 0:
        return "0 (exact on the grid)"
    return f"2^{float(mpmath.log(value, 2)):.2f}"


class Polynomial:
    """One fitted table: what it approximates, how the kernel uses it, and how to measure it."""

    def __init__(self, name, doc, fitted, interval, target, reduced, exact, integer, grid):
        self.name = name
        self.doc = doc
        self.fitted = fitted          # the function chebyfit approximates, of the poly variable
        self.interval = interval      # where it is fitted
        self.target = target          # the fit error the degree has to reach
        self.reduced = reduced        # grid point -> the poly variable and the exact reduced value
        self.exact = exact            # (coefficients, grid point) -> the reduced value, exactly
        self.integer = integer        # (kernel, grid point) -> the kernel's reduced value, Q2.62
        self.grid = grid              # the grid points

    def fit(self, count: int) -> list[int]:
        coefficients = mpmath.chebyfit(self.fitted, self.interval, count)
        return quantize(list(reversed(coefficients)))

    def fit_error(self, coefficients: list[int]) -> mpf:
        worst = mpf(0)
        for point in self.grid:
            worst = max(worst, abs(self.exact(coefficients, point) - self.reduced(point)))
        return worst

    def evaluated_error(self, kernel: model.Kernel) -> mpf:
        worst = mpf(0)
        for point in self.grid:
            worst = max(worst, abs(mpf(self.integer(kernel, point)) / SCALE - self.reduced(point)))
        return worst


QUARTER_PI = mp.pi / 4
TAN_PI_8 = mp.tan(mp.pi / 8)
LOG_S_MAX = 3 - 2 * mp.sqrt(2)


def _grid(count: int, last: int) -> list[int]:
    return sorted({(last * index) // count for index in range(count + 1)})


def polynomials() -> list[Polynomial]:
    octant_grid = _grid(GRID, 1 << 29)
    t_of = lambda x: mpf(x) / (1 << 29)  # noqa: E731

    sine = Polynomial(
        "kSin",
        "sin(pi/4 * t) = t * P(t^2) for t in [0, 1]: the sine half of one octant (design §5.2).",
        lambda u: mp.sin(QUARTER_PI * mp.sqrt(u)) / mp.sqrt(u),
        [0, 1], mpf(2) ** -44,
        lambda x: mp.sin(QUARTER_PI * t_of(x)),
        lambda c, x: t_of(x) * exact_poly(c, t_of(x) ** 2),
        lambda k, x: k._octant_cores(x)[1],
        octant_grid)

    cosine = Polynomial(
        "kCos",
        "cos(pi/4 * t) = 1 + t^2 * P(t^2) for t in [0, 1]. The constant 1 is exact and not stored.",
        lambda u: (mp.cos(QUARTER_PI * mp.sqrt(u)) - 1) / u,
        [0, 1], mpf(2) ** -44,
        lambda x: mp.cos(QUARTER_PI * t_of(x)),
        lambda c, x: 1 + t_of(x) ** 2 * exact_poly(c, t_of(x) ** 2),
        lambda k, x: k._octant_cores(x)[2],
        octant_grid)

    atan_points = _grid(GRID, int(mpmath.floor(TAN_PI_8 * SCALE)))
    atan = Polynomial(
        "kAtan",
        "atan(z) / (2 pi) = z * P(z^2) for |z| <= tan(pi/8), in turns.",
        lambda u: mp.atan(mp.sqrt(u)) / (2 * mp.pi * mp.sqrt(u)),
        [0, TAN_PI_8 ** 2], mpf(2) ** -44,
        lambda z: mp.atan(mpf(z) / SCALE) / (2 * mp.pi),
        lambda c, z: (mpf(z) / SCALE) * exact_poly(c, (mpf(z) / SCALE) ** 2),
        lambda k, z: k.mul62(k.horner(k.t["kAtan"], k.mul62(z, z)), z),
        atan_points)

    exp2_points = _grid(GRID, (1 << 62) - 1)
    exp2 = Polynomial(
        "kExp2",
        "2^f = 1 + f * P(f) for f in [0, 1). The constant 1 is exact and not stored.",
        lambda f: (mpf(2) ** f - 1) / f,
        [0, 1], mpf(2) ** -46,
        lambda f: mpf(2) ** (mpf(f) / SCALE),
        lambda c, f: 1 + (mpf(f) / SCALE) * exact_poly(c, mpf(f) / SCALE),
        lambda k, f: model.wrap64(model.ONE62 + k.mul62(k.horner(k.t["kExp2"], f), f)),
        exp2_points)

    log_points = _grid(GRID, int(mpmath.floor(LOG_S_MAX * SCALE)))
    log2 = Polynomial(
        "kLog2",
        "log2((1 + s) / (1 - s)) = 4 * s * P(s^2) for |s| <= 3 - 2 sqrt(2). Stored divided by four "
        "so that P(0) = 2 / (4 ln 2) fits Q2.62; the kernel multiplies with a shift of 60, not 62.",
        lambda u: mp.log((1 + mp.sqrt(u)) / (1 - mp.sqrt(u)), 2) / (4 * mp.sqrt(u)),
        [0, LOG_S_MAX ** 2], mpf(2) ** -44,
        lambda s: mp.log((1 + mpf(s) / SCALE) / (1 - mpf(s) / SCALE), 2),
        lambda c, s: 4 * (mpf(s) / SCALE) * exact_poly(c, (mpf(s) / SCALE) ** 2),
        lambda k, s: k.mulshift(k.horner(k.t["kLog2"], k.mul62(s, s)), s, 60),
        log_points)

    return [sine, cosine, atan, exp2, log2]


def constants() -> list[tuple[str, str, str, int]]:
    """(type, name, doc, value). Each is the correctly rounded integer nearest the real constant."""
    nint = lambda value: int(mpmath.nint(value))  # noqa: E731
    return [
        ("i64", "kTanPiOver8Q62", "tan(pi/8) in Q2.62: where atan2's second reduction begins.",
         nint(TAN_PI_8 * SCALE)),
        ("i64", "kLog2eQ62", "log2(e) in Q2.62, for exp(x) = exp2(x log2 e).",
         nint(mp.log(mp.e, 2) * SCALE)),
        ("i64", "kLn2Q62", "ln(2) in Q2.62, for log(x) = log2(x) ln 2.", nint(mp.log(2) * SCALE)),
        ("i64", "kInvTwoPiQ64", "1/(2 pi) in Q0.64, for radians to turns.",
         nint(mpf(2) ** 64 / (2 * mp.pi))),
        ("i64", "kTwoPiQ60", "2 pi in Q4.60, for turns to radians.", nint(2 * mp.pi * mpf(2) ** 60)),
        ("i64", "kPiQ32", "pi as a Fixed.", nint(mp.pi * mpf(2) ** 32)),
        ("i64", "kTwoPiQ32", "2 pi as a Fixed.", nint(2 * mp.pi * mpf(2) ** 32)),
        ("i64", "kHalfPiQ32", "pi / 2 as a Fixed.", nint(mp.pi / 2 * mpf(2) ** 32)),
        ("i64", "kEQ32", "e as a Fixed.", nint(mp.e * mpf(2) ** 32)),
    ]


def render(tables: list[tuple[Polynomial, list[int], mpf, mpf]], scalars) -> str:
    lines = [
        "// SPDX-License-Identifier: MIT",
        "#pragma once",
        "// GENERATED by tools/detmath/gen_coefficients.py. Do not edit: `just generate-check` fails",
        "// when this file is not what the generator writes, and tools/detmath/gen_coefficients.py",
        "// states the method, the targets and the reason they are set where they are.",
        "//",
        "// Every polynomial is stored LOW ORDER FIRST, in Q2.62, and evaluated by Horner's rule with",
        "// the kernel's `mul62` (round to nearest, ties toward +infinity). `fit` is the error of the",
        "// rounded coefficients evaluated exactly; `evaluated` is the error of the kernel's own",
        "// integer evaluation, measured through tools/detmath/model.py. Both are maxima over a",
        f"// {GRID + 1}-point grid of the reduced argument, against mpmath at {mp.dps} digits.",
        "//",
        "// A change to any value here changes what the engine computes, so it bumps",
        "// `detmath::kKernelVersion` (version.h) in the same change.",
        "",
        "#include <cy/core/base/types.h>",
        "#include <cy/core/detmath/config.h>",
        "",
        "namespace cy::detmath::inline CY_DETMATH_VARIANT::coefficients {",
        "",
        "// One coefficient per line, as the generator writes them, so a regenerated table diffs line",
        "// by line.",
        "// clang-format off",
        "",
    ]
    for polynomial, coefficients, fit, evaluated in tables:
        degree = len(coefficients) - 1
        lines.extend(textwrap.wrap(polynomial.doc, 96, initial_indent="/// ",
                                   subsequent_indent="/// "))
        summary = (f"{len(coefficients)} coefficients (degree {degree} in the polynomial's "
                   f"variable); fit {log2_of(fit)}, evaluated {log2_of(evaluated)}, target "
                   f"{log2_of(polynomial.target)}.")
        lines.extend(textwrap.wrap(summary, 96, initial_indent="/// ", subsequent_indent="/// "))
        lines.append(f"inline constexpr i64 {polynomial.name}[] = {{")
        for coefficient in coefficients:
            lines.append(f"    {coefficient}LL,")
        lines.append("};")
        lines.append("")
    for kind, name, doc, value in scalars:
        lines.append(f"/// {doc}")
        lines.append(f"inline constexpr {kind} {name} = {value}LL;")
        lines.append("")
    lines.append("// clang-format on")
    lines.append("")
    lines.append("}  // namespace cy::detmath::inline CY_DETMATH_VARIANT::coefficients")
    lines.append("")
    return "\n".join(lines)


def generate() -> str:
    scalars = constants()
    table: dict[str, object] = {name: value for _, name, _, value in scalars}
    chosen: list[tuple[Polynomial, list[int], mpf]] = []
    for polynomial in polynomials():
        for count in range(2, 16):
            coefficients = polynomial.fit(count)
            fit = polynomial.fit_error(coefficients)
            if fit <= polynomial.target:
                break
        else:
            raise SystemExit(f"{polynomial.name}: no degree below 15 reaches the target")
        table[polynomial.name] = coefficients
        chosen.append((polynomial, coefficients, fit))
    kernel = model.Kernel(table)
    measured = [(p, c, fit, p.evaluated_error(kernel)) for p, c, fit in chosen]
    return render(measured, scalars)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail when the header is stale")
    parser.add_argument("--output", type=pathlib.Path, default=model.COEFFICIENT_HEADER)
    arguments = parser.parse_args()

    text = generate()
    if arguments.check:
        current = arguments.output.read_text(encoding="utf-8") if arguments.output.exists() else ""
        if current != text:
            sys.stdout.writelines(difflib.unified_diff(
                current.splitlines(True), text.splitlines(True), str(arguments.output), "generated"))
            print(f"\n{arguments.output} is stale: run python3 tools/detmath/gen_coefficients.py "
                  "and bump detmath::kKernelVersion if any value moved", file=sys.stderr)
            return 1
        print(f"{arguments.output}: current")
        return 0
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(text, encoding="utf-8")
    print(f"wrote {arguments.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
