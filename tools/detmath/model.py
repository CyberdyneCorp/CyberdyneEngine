#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The deterministic math kernel, written a second time in Python integers.

`openspec/changes/add-deterministic-math/design.md` §4.3 says the arithmetic rules ARE the
specification and that every implementation must produce the same bits. This file is the second
implementation. It is written from the rules, not translated from the C++ line by line: Python's
integers are unbounded, so a product here is simply `a * b` and a wrap is a mask, while the C++ has
to assemble the same answer out of 64-bit limbs. When the two agree on every golden input, the
agreement is evidence about the rules rather than about one way of writing them.

It is read by three tools:

    gen_coefficients.py   evaluates each candidate polynomial exactly as the kernel will, to measure
                          the error the integer evaluation really has
    gen_vectors.py        computes every expected output in tools/detmath/vectors/
    gen_oracle.py         nothing: the oracle is mpmath, and deliberately shares no code with this

Every function takes and returns raw two's-complement integers: a `Fixed` is its `i64` raw value
(Q32.32), an `Angle` is its `u32` raw value (a full turn is 2**32).
"""

from __future__ import annotations

import pathlib
import re

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
COEFFICIENT_HEADER = (REPO / "src/core/detmath/include/cy/core/detmath/generated/coefficients.h")

MASK64 = (1 << 64) - 1
MASK32 = (1 << 32) - 1
I64_MAX = (1 << 63) - 1
I64_MIN = -(1 << 63)
ONE = 1 << 32
ONE62 = 1 << 62
FRACTION62 = ONE62 - 1


def wrap64(value: int) -> int:
    """Two's-complement wrap to a signed 64-bit integer."""
    value &= MASK64
    return value - (1 << 64) if value >> 63 else value


def wrap32u(value: int) -> int:
    return value & MASK32


def u64(value: int) -> int:
    return value & MASK64


# --- The coefficient table, read from the generated header --------------------------------------
#
# The header is the single place the coefficients live. Reading it back here, rather than importing
# them from the generator, means the model evaluates exactly the integers the C++ compiles.

_TABLE_RE = re.compile(r"inline constexpr i64 (k\w+)\[\] = \{([^}]*)\};")
_SCALAR_RE = re.compile(r"inline constexpr (?:i64|u64) (k\w+) = (-?\d+)(?:LL|ULL)?;")


def _integer(text: str) -> int:
    text = text.strip().rstrip("LU")
    return int(text)


def load_coefficients(path: pathlib.Path = COEFFICIENT_HEADER) -> dict[str, object]:
    source = path.read_text(encoding="utf-8")
    table: dict[str, object] = {}
    for name, body in _TABLE_RE.findall(source):
        table[name] = [_integer(item) for item in body.replace("\n", " ").split(",") if item.strip()]
    for name, value in _SCALAR_RE.findall(source):
        table[name] = int(value)
    return table


class Kernel:
    """The kernel's functions over one coefficient table."""

    def __init__(self, table: dict[str, object]) -> None:
        self.t = table

    # --- scalar arithmetic (design §4.3) ---------------------------------------------------------

    @staticmethod
    def add(a: int, b: int) -> int:
        return wrap64(a + b)

    @staticmethod
    def sub(a: int, b: int) -> int:
        return wrap64(a - b)

    @staticmethod
    def mul(a: int, b: int) -> int:
        # The exact product, plus half an ulp, floored: round to nearest, ties toward +infinity.
        return wrap64((a * b + (1 << 31)) >> 32)

    @staticmethod
    def div(a: int, b: int) -> int:
        if b == 0:
            return I64_MAX if a >= 0 else I64_MIN
        numerator = a << 32
        quotient = abs(numerator) // abs(b)
        if (numerator < 0) != (b < 0):
            quotient = -quotient
        return wrap64(quotient)

    @staticmethod
    def narrow16(a: int) -> int:
        """`Fixed` to `Fixed16`: round to nearest, ties toward +infinity, saturate."""
        rounded = (a + (1 << 15)) >> 16
        return max(-(1 << 31), min((1 << 31) - 1, rounded))

    @staticmethod
    def angle_scale(angle: int, k: int) -> int:
        return wrap32u((angle * k + (1 << 31)) >> 32)

    def angle_from_radians(self, radians: int) -> int:
        product = radians * self.t["kInvTwoPiQ64"]
        return wrap32u((product + (1 << 63)) >> 64)

    def angle_radians(self, angle: int) -> int:
        product = angle * self.t["kTwoPiQ60"]
        return wrap64((product + (1 << 59)) >> 60)

    # --- square root -----------------------------------------------------------------------------

    @staticmethod
    def sqrt_wide(raw128: int) -> int:
        """The square root of a Q64.64 value as a Q32.32 one, correctly rounded."""
        if raw128 < 0:
            return 0
        root = _isqrt(raw128)
        if raw128 - root * root > root:
            root += 1
        return min(root, I64_MAX)

    def sqrt(self, a: int) -> int:
        if a < 0:
            return 0
        return self.sqrt_wide(a << 32)

    # --- kernel helpers --------------------------------------------------------------------------

    @staticmethod
    def mulshift(a: int, b: int, shift: int) -> int:
        return wrap64((a * b + (1 << (shift - 1))) >> shift)

    def mul62(self, a: int, b: int) -> int:
        return self.mulshift(a, b, 62)

    def horner(self, coefficients: list[int], x: int) -> int:
        value = coefficients[-1]
        for coefficient in reversed(coefficients[:-1]):
            value = wrap64(self.mul62(value, x) + coefficient)
        return value

    # --- sine and cosine -------------------------------------------------------------------------

    def _octant_cores(self, angle: int) -> tuple[int, int, int]:
        octant = angle >> 29
        rest = angle & ((1 << 29) - 1)
        x = (1 << 29) - rest if octant & 1 else rest
        t = x << 33
        u = self.mul62(t, t)
        sine = self.mul62(self.horner(self.t["kSin"], u), t)
        cosine = wrap64(ONE62 + self.mul62(self.horner(self.t["kCos"], u), u))
        return octant, sine, cosine

    #: For each octant: (sine comes from the cosine core, sine negated, cosine negated).
    _OCTANTS = (
        (False, False, False),
        (True, False, False),
        (True, False, True),
        (False, False, True),
        (False, True, True),
        (True, True, True),
        (True, True, False),
        (False, True, False),
    )

    def sincos62(self, angle: int) -> tuple[int, int]:
        octant, s, c = self._octant_cores(angle)
        swap, sine_negative, cosine_negative = self._OCTANTS[octant]
        sine, cosine = (c, s) if swap else (s, c)
        return (-sine if sine_negative else sine), (-cosine if cosine_negative else cosine)

    @staticmethod
    def _to_fixed_signed(value62: int) -> int:
        magnitude = (abs(value62) + (1 << 29)) >> 30
        return -magnitude if value62 < 0 else magnitude

    def sin(self, angle: int) -> int:
        return self._to_fixed_signed(self.sincos62(angle)[0])

    def cos(self, angle: int) -> int:
        return self._to_fixed_signed(self.sincos62(angle)[1])

    def tan(self, angle: int) -> int:
        sine, cosine = self.sincos62(angle)
        if cosine == 0:
            return I64_MAX if sine >= 0 else I64_MIN
        quotient = (abs(sine) << 32) // abs(cosine)
        negative = (sine < 0) != (cosine < 0)
        if quotient > I64_MAX:
            return I64_MIN if negative else I64_MAX
        return -quotient if negative else quotient

    # --- arctangent ------------------------------------------------------------------------------

    def atan2(self, y: int, x: int) -> int:
        if x == 0 and y == 0:
            return 0
        ax, ay = abs(x), abs(y)
        swap = ay > ax
        numerator, denominator = (ax, ay) if swap else (ay, ax)
        ratio = (numerator << 62) // denominator
        if ratio > self.t["kTanPiOver8Q62"]:
            z = -(((ONE62 - ratio) << 62) // (ONE62 + ratio))
            base = 1 << 59
        else:
            z = ratio
            base = 0
        u = self.mul62(z, z)
        turns = base + self.mul62(self.horner(self.t["kAtan"], u), z)
        angle = (turns + (1 << 29)) >> 30
        if swap:
            angle = (1 << 30) - angle
        if x < 0:
            angle = (1 << 31) - angle
        if y < 0:
            angle = -angle
        return wrap32u(angle)

    def atan(self, x: int) -> int:
        return self.atan2(x, ONE)

    def _cosine_of_sine(self, x: int) -> tuple[int, int]:
        clamped = max(-ONE, min(ONE, x))
        return clamped, self.sqrt_wide((1 << 64) - clamped * clamped)

    def asin(self, x: int) -> int:
        clamped, cosine = self._cosine_of_sine(x)
        return self.atan2(clamped, cosine)

    def acos(self, x: int) -> int:
        clamped, sine = self._cosine_of_sine(x)
        return self.atan2(sine, clamped)

    # --- exponentials and logarithms -------------------------------------------------------------

    def exp2_core(self, n: int, f62: int) -> int:
        value = wrap64(ONE62 + self.mul62(self.horner(self.t["kExp2"], f62), f62))
        if n >= 31:
            return I64_MAX
        shift = 30 - n
        if shift >= 64:
            return 0
        if shift == 0:
            return value
        return (value + (1 << (shift - 1))) >> shift

    def exp2(self, x: int) -> int:
        return self.exp2_core(x >> 32, (x & MASK32) << 30)

    def log2_core(self, x: int) -> tuple[int, int]:
        """`log2(x)` as an integer exponent and a Q2.62 remainder in [-1/2, 1/2]."""
        top = x.bit_length() - 1
        if x * x >= 1 << (2 * top + 1):
            numerator, denominator, negative, exponent = (1 << (top + 1)) - x, x + (1 << (top + 1)), True, top - 31
        else:
            numerator, denominator, negative, exponent = x - (1 << top), x + (1 << top), False, top - 32
        s = (numerator << 62) // denominator
        u = self.mul62(s, s)
        magnitude = self.mulshift(self.horner(self.t["kLog2"], u), s, 60)
        return exponent, (-magnitude if negative else magnitude)

    def log2(self, x: int) -> int:
        if x <= 0:
            return I64_MIN
        exponent, remainder = self.log2_core(x)
        return wrap64((exponent << 32) + ((remainder + (1 << 29)) >> 30))

    def exp(self, x: int) -> int:
        scaled = (x * self.t["kLog2eQ62"] + (1 << 31)) >> 32
        return self.exp2_core(scaled >> 62, scaled & FRACTION62)

    def log(self, x: int) -> int:
        if x <= 0:
            return I64_MIN
        exponent, remainder = self.log2_core(x)
        ln2 = self.t["kLn2Q62"]
        total = exponent * ln2 + ((remainder * ln2 + (1 << 61)) >> 62)
        return wrap64((total + (1 << 29)) >> 30)

    def pow(self, x: int, y: int) -> int:
        if x <= 0:
            return 0
        exponent, remainder = self.log2_core(x)
        scaled = ((y * exponent) << 30) + ((y * remainder + (1 << 31)) >> 32)
        return self.exp2_core(scaled >> 62, scaled & FRACTION62)


def _isqrt(value: int) -> int:
    import math

    return math.isqrt(value)


# --- The seeded sweep, shared with src/core/detmath/src/digest.cpp ------------------------------

SWEEP_SEED = 0xDE7_3A7_0001
SWEEP_COUNT = 16384
GOLDEN_SEEDED = 256


class SplitMix64:
    def __init__(self, seed: int) -> None:
        self.state = seed & MASK64

    def next(self) -> int:
        self.state = (self.state + 0x9E3779B97F4A7C15) & MASK64
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
        return z ^ (z >> 31)


def finalize(value: int) -> int:
    z = value & MASK64
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
    return z ^ (z >> 31)


def fold(accumulator: int, value: int) -> int:
    """Order-sensitive: the same outputs in another order fold to another digest."""
    return finalize(((accumulator ^ (value & MASK64)) + 0x9E3779B97F4A7C15) & MASK64)


def scaled(rng: SplitMix64) -> int:
    """A raw value whose magnitude is spread over every binade rather than clustered at the top."""
    return wrap64(rng.next()) >> (rng.next() & 63)


def ranged(rng: SplitMix64, low_raw: int, high_raw: int) -> int:
    return low_raw + rng.next() % (high_raw - low_raw)


def positive(rng: SplitMix64) -> int:
    value = scaled(rng) & I64_MAX
    return value if value != 0 else 1


#: Every function the kernel digest covers, in digest order, with the arity of its inputs. The
#: order and the draws are part of the digest's definition; digest.cpp states them identically.
FUNCTIONS = (
    "add", "sub", "mul", "div", "sqrt", "sqrt_wide", "narrow16", "angle_scale",
    "angle_from_radians", "angle_radians", "sin", "cos", "tan", "atan", "atan2", "asin", "acos",
    "exp2", "log2", "exp", "log", "pow",
)


def draw(function: str, rng: SplitMix64) -> tuple[int, ...]:
    """The inputs of one sweep step. Raw integers: i64 for a Fixed, u32 for an Angle, and a
    128-bit value for `sqrt_wide`, drawn as (high, low)."""
    if function in ("add", "sub"):
        return wrap64(rng.next()), wrap64(rng.next())
    if function in ("mul", "div"):
        return scaled(rng), scaled(rng)
    if function == "sqrt":
        return (scaled(rng),)
    if function == "sqrt_wide":
        high = wrap64(rng.next()) >> (rng.next() & 63)
        low = rng.next()
        return high, low
    if function == "narrow16":
        return (scaled(rng),)
    if function == "angle_scale":
        return rng.next() >> 32, scaled(rng) >> 16
    if function == "angle_from_radians":
        return (scaled(rng),)
    if function == "angle_radians":
        return (rng.next() >> 32,)
    if function in ("sin", "cos", "tan"):
        return (rng.next() >> 32,)
    if function == "atan":
        return (scaled(rng),)
    if function == "atan2":
        return scaled(rng), scaled(rng)
    if function in ("asin", "acos"):
        return (ranged(rng, -(5 << 30), 5 << 30),)
    if function in ("exp2", "exp"):
        return (ranged(rng, -(40 << 32), 40 << 32),)
    if function in ("log2", "log"):
        return (positive(rng),)
    if function == "pow":
        return positive(rng), ranged(rng, -(8 << 32), 8 << 32)
    raise KeyError(function)


def evaluate(kernel: Kernel, function: str, inputs: tuple[int, ...]) -> int:
    """One function on raw inputs. The result is returned as its unsigned 64-bit bit pattern."""
    if function == "sqrt_wide":
        high, low = inputs
        result = kernel.sqrt_wide((high << 64) | low)
    elif function == "narrow16":
        result = kernel.narrow16(inputs[0])
    else:
        result = getattr(kernel, function)(*inputs)
    return u64(result)


def function_seed(index: int) -> int:
    return (SWEEP_SEED + 0x1000 * (index + 1)) & MASK64


def function_digest(kernel: Kernel, function: str, count: int = SWEEP_COUNT) -> int:
    index = FUNCTIONS.index(function)
    rng = SplitMix64(function_seed(index))
    accumulator = function_seed(index)
    for _ in range(count):
        accumulator = fold(accumulator, evaluate(kernel, function, draw(function, rng)))
    return accumulator


def kernel_digest(per_function: dict[str, int], version: int) -> int:
    accumulator = fold(SWEEP_SEED, version)
    for function in FUNCTIONS:
        accumulator = fold(accumulator, per_function[function])
    return accumulator
