# Deterministic math in CyberEngine

A guide for a gameplay or engine contributor who needs a result that is the same bits on every
machine: what `cy::core-detmath` is, which type to use for what, the rules every operation follows,
how accurate the transcendentals are and how that is proven, where floats may and may not cross into
it, and how the module is checked on each CI leg and between them.

**Governed by**: the `deterministic-math` capability and the deltas of
[`openspec/changes/add-deterministic-math`](../../openspec/changes/add-deterministic-math/proposal.md),
with [`simulation-and-determinism`](../../openspec/specs/simulation-and-determinism/spec.md) for the
profiles. The module's [README](../../src/core/detmath/README.md) is the detailed reference.

| Where | What |
|---|---|
| [`src/core/detmath/`](../../src/core/detmath/README.md) | The module: `Fixed`, `Fixed16`, `Angle`, `WideFixed`, the functions, the conversion boundary |
| [`tools/detmath/`](../../tools/detmath/) | The coefficient generator, the Python model of the rules, the golden vectors and the oracle |
| [`benchmarks/detmath/`](../../benchmarks/detmath/bench_detmath.cpp) | The costs against the design's budgets |
| [`tests/determinism/test_cross_leg.cpp`](../../tests/determinism/test_cross_leg.cpp) | Publishes the kernel digest for the four-leg comparison |

![How far each function's worst measured error sits inside its declared bound, and the error of sin
across a quarter turn](../design/images/detmath-errors.png)

*Left: each function's worst error against the mpmath oracle, as a fraction of its declared bound.
Right: the error of `sin` over a quarter turn, in ulps — the final rounding's ±0.5 ulp band, with the
polynomial's own error invisible beneath it. Drawn by `tools/detmath/plot_errors.py` from the
committed oracle and the model the golden vectors hold the C++ to.*

## Contents

1. [Why fixed point](#1-why-fixed-point)
2. [The types](#2-the-types)
3. [The rules](#3-the-rules)
4. [The functions and their bounds](#4-the-functions-and-their-bounds)
5. [The conversion boundary](#5-the-conversion-boundary)
6. [Profiles: what linking the module changes](#6-profiles-what-linking-the-module-changes)
7. [How it is proven](#7-how-it-is-proven)
8. [Performance](#8-performance)
9. [Changing the kernel](#9-changing-the-kernel)
10. [Pitfalls](#10-pitfalls)
11. [What is not built yet](#11-what-is-not-built-yet)

## 1. Why fixed point

Strict IEEE floats are deterministic only if every translation unit on the path is compiled the same
way — contraction, fast-math, the libm's transcendentals, the per-thread denormal mode, the Swift
compiler's choices. The engine controls that for its own code (`SamePlatform`) and cannot control it
for everything an authoritative path may touch. Integer arithmetic has none of those knobs: C++20
defines it exactly, including two's-complement conversion. So `CrossPlatform` and `Lockstep`
arithmetic is fixed point, and the transcendentals are written in the same arithmetic.

## 2. The types

| Type | Storage | Use it for |
|---|---|---|
| `Fixed` | Q32.32 in an `i64`; range ±2^31, resolution 2^-32 | **Every authoritative quantity**: positions (world-absolute), velocities, times, distances, costs |
| `Fixed16` | Q16.16 in an `i32` | Storage only, for dense data whose range fits. `widen()` to compute |
| `Angle` | a `u32`, a full turn = 2^32 | Headings and rotations. Wrapping is free and exact |
| `WideFixed` | Q64.64 in 128 bits | Products, squared lengths and dot products, so a comparison of distances never overflows |

```cpp
#include <cy/core/detmath/detmath.h>

using cy::detmath::Angle;
using cy::detmath::Fixed;
using cy::detmath::WideFixed;
namespace dm = cy::detmath;

// Distance comparison without overflow, and without a square root.
const Fixed dx = target_x - unit_x;
const Fixed dz = target_z - unit_z;
const WideFixed d2 = WideFixed::product(dx, dx) + WideFixed::product(dz, dz);
const bool in_range = d2 <= WideFixed::product(range, range);

// A step toward the target.
const Angle heading = dm::atan2(dz, dx);
const dm::SinCos dir = dm::sincos(heading);
unit_x += dir.cos * speed * dt;
unit_z += dir.sin * speed * dt;
```

Units are those of `core-math` — metres, seconds, kilograms — except `Angle`, which is turns.

## 3. The rules

The rules are the specification; the C++, the Python model in `tools/detmath/model.py` and (later)
Swift must produce the same bits.

| Operation | Rule |
|---|---|
| `+`, `-`, unary `-` | Two's-complement wrap through `u64` |
| `*` | Exact 128-bit product, round to nearest with **ties toward +∞**, wrap |
| `/` | `(a << 32) / b` **truncated toward zero**, wrap. `x / 0` is `max()` for `x >= 0`, `min()` for `x < 0` |
| `>>` | Arithmetic (floor) |
| Comparison | Raw integer; total |
| `Fixed16::narrow` | Round to nearest, ties toward +∞, saturate |
| `Angle` | `u32` wrap; `Angle * Fixed` through 128 bits |

**Overflow wraps.** It does not saturate (that would turn a bug into plausible gameplay) and it does
not trap (development and shipping would compute different things). Wrapping is deterministic, so
peers that overflow still agree. A development build counts it:

```cpp
dm::OverflowGuard guard;          // installs itself on this thread
step_authoritative_units(world);
if (guard.overflows() != 0) {
    report_overflow(guard.overflows(), guard.first_site());   // first_site() is e.g. "Fixed *"
}
```

`saturating_add`, `saturating_sub` and `saturating_mul` exist for clamps that want saturation.

## 4. The functions and their bounds

Errors are against the exact real function **of the quantised input**; an ulp is 2^-32 of the
output. Each bound is asserted by `integration.detmath_vectors` against an mpmath oracle, which prints
the worst error beside it.

| Function | Reduction | Declared bound | Worst measured |
|---|---|---|---|
| `sqrt(Fixed)`, `sqrt(WideFixed)` | none: integer root of a 128-bit value | 0.5 ulp (correctly rounded) | 0.50 |
| `sin`, `cos`, `sincos` | octant from the angle's top three bits, exact | 1 ulp | 0.50 |
| `tan` | the sine and cosine cores, divided | 2 ulp where \|tan\| ≤ 1; relative 2^-30 above | 1.0 |
| `atan`, `atan2` | octant from signs and \|y\| vs \|x\|, then about tan(π/8) | 1 ulp of `Angle` | 0.50 |
| `asin`, `acos` | `atan2(x, sqrt(1 - x²))` through `WideFixed` | 2 ulp of `Angle`; 2^-15 turn within 2^-16 of ±1 | 0.56 |
| `exp2` | integer part a shift, fraction a polynomial | max(1 ulp, relative 2^-36) | 0.50 |
| `log2` | `countl_zero`, an exact √2 test, s = (m − 1)/(m + 1) | 2 ulp | 0.50 |
| `exp`, `log` | `exp2`/`log2` kept at 2^-62 internally | max(2 ulp, rel. 2^-34); 3 ulp | 0.50; 0.50 |
| `pow(x > 0, y)` | `exp2(y log2 x)` at 2^-62 | max(2 ulp, rel. 2^-34 + \|y\| 2^-58) | 0.50 |

Exact identities, asserted with `==`: `sin(a + quarter) == cos(a)`, `sin(-a) == -sin(a)`,
`atan2(-y, x) == -atan2(y, x)`, `exp2(n) == 2^n`, `log2(2^n) == n`.

Outside a domain, every function answers something defined and counts it: `sqrt` of a negative is 0;
`log2`/`log` of a non-positive is `min()`; `pow` of a non-positive base is 0; `asin`/`acos` clamp;
`exp2`/`exp`/`pow`/`tan` saturate; `atan2(0, 0)` is 0.

## 5. The conversion boundary

Float becomes `Fixed` at three moments only, and never inside a tick:

| Moment | Call |
|---|---|
| Cook | `from_f32_cooked` / `from_f64_cooked`; the asset stores the raw `i64` |
| Session configuration | the same, before tick 0, folded into the state hash |
| Command creation | the issuing peer converts its float pick; the command log carries the raw value |

The conversion multiplies by 2^32 (exact) and rounds with `std::nearbyint` — ties to even, an
operation IEEE 754 defines exactly — so even it is the same everywhere. Out of range saturates; NaN
is 0; both are counted. The audit is a grep: `grep -rn "from_f32_cooked\|from_f64_cooked" src`.

`Fixed` becomes a float for presentation only: `to_f32_relative(value, camera)` subtracts in `Fixed`
first, so a unit two million metres out is still exact to its last bit relative to the camera.

## 6. Profiles: what linking the module changes

`cy::core-detmath` exports `CY_DETERMINISM_MATH=1` **PUBLIC**. Any translation unit that links it
sees `BuildConfiguration::from_build().deterministic_math_available`, and
`DeterminismConfiguration::require()` then stops refusing `CrossPlatform` and `Lockstep` with
`DeterministicMathMissing`. It goes on to the subsystems, in name order, and refuses the first
authoritative one that declares less:

```text
require(Lockstep)  without the module  ->  DeterministicMathMissing
require(Lockstep)  with the module     ->  SubsystemGuarantee, subsystem "physics",
                                           "cross-platform reproducibility"
```

The module alone accepts nothing. Today every float-based subsystem — Jolt-authoritative physics,
abilities, AI utility, root motion — still declares `SamePlatform`, so a session using one
authoritatively is refused by name. The cases are `unit.determinism`'s three
"CrossPlatform is ..." cases and `unit.detmath`'s two profile cases.

## 7. How it is proven

| Evidence | Where | Fails when |
|---|---|---|
| The rules, case by case | `unit.detmath` | a tie, a truncation, a wrap or a saturation is wrong |
| The 128-bit paths against the 32-bit-limb reference | `unit.detmath`, and a 20 000-input sweep in `integration.detmath_vectors` | an intrinsic disagrees on the leg that uses it |
| Golden vectors and per-function digests | `integration.detmath_vectors`, on **each leg alone** | this leg's C++ disagrees with the Python model of the rules |
| Declared bounds against mpmath | `integration.detmath_vectors` | a function leaves its bound |
| No float in the kernel | the `-mgeneral-regs-only` build in `integration.detmath_variants` | a float reaches a kernel source (the build fails) |
| Two vector widths, one answer | the `-mavx2` build in `integration.detmath_variants` | any function's digest differs |
| Four legs, one answer | `determinism.cross_leg` publishes `detmath-kernel-digest`; `cross-leg-compare` runs `--detmath` | two architectures disagree |

The golden vectors and digests come from `tools/detmath/model.py`, the rules written a second time
in Python integers; the oracle comes from mpmath and shares no code with either. Run them:

```sh
just test-unit -R detmath
just test-integration -R detmath
just test-determinism -R cross_leg
just test-determinism --compare-legs --pcg --detmath --digests cross-leg-digests
```

## 8. Performance

`just test-bench` runs `benchmarks/detmath/`, one dependent chain per operation, against the
budgets of design §11 (x86-64 reference runner):

| Operation | Budget | First measurement |
|---|---|---|
| `*` | ≤ 2× an `f64` multiply in a dependent chain (`detmath/f64-mul`) | 2.18 ns, 2.00× the `f64` multiply |
| `/` | ≤ 30 ns | 9.2 ns |
| `sqrt` | ≤ 40 ns | 24.6 ns |
| `sin`, `cos` | ≤ 25 ns | 19.7 ns |
| `atan2` | ≤ 50 ns | 30.0 ns |
| `exp2`, `log2` | ≤ 30 ns each | 26.8 ns, 26.0 ns |

The first measurement is the `profile` build on a 24-core i9-12900K workstation under a load of about
ten, three runs of nine repetitions agreeing within 2 %; the design's budgets are stated for the CI
x86-64 reference runner, and the committed thresholds are ratios to the harness's calibration
workload, so they carry across machines. `*` sits exactly at its budget: the exact product, the
rounding add and the shift are a dependent chain of about eight cycles against the multiply's four,
and that is the cost of a rounding rule every implementation can reproduce. It took GCC's and
Clang's signed 128-bit multiply to get there; the product built from the unsigned one (MSVC's path)
measured 2.7×.

The measured figures and the committed thresholds are in
[`benchmarks/baseline.json`](../../benchmarks/baseline.json).

## 9. Changing the kernel

Any change that moves any output for any input — a coefficient, a reduction, a rounding rule —
bumps `detmath::kKernelVersion` (`version.h`) in the same change. The kernel version is folded into
the digest and published by every leg; later stages record it in replays and lockstep scopes so a
mismatch is a refusal at join, not a desync.

```sh
just generate-detmath            # coefficients, vectors, oracle (needs mpmath==1.3.0)
just generate-detmath --check    # what `just generate-check` runs in CI
CY_DETMATH_RECORD_GOLDEN=1 just test-integration -R detmath_vectors   # rewrites, and FAILS
```

## 10. Pitfalls

- **Squaring a distance in `Fixed`.** `d * d` overflows beyond 46 341 m. Use `WideFixed::product`.
- **Dividing by a value that may be zero.** It does not trap; it saturates and is counted. Test the
  divisor, or multiply by a reciprocal computed once.
- **Converting per tick.** A `from_f32_cooked` inside a system is a float on the authoritative path.
  It is legal C++ and it defeats the module.
- **Comparing angles with `<`.** `Angle` has no order; compare `signed_turns()` of a difference.
- **Swift's `+`.** It traps on overflow; the engine's rule is wrapping. The Swift `Fixed` (a later
  stage) uses `&+`.

## 11. What is not built yet

Stages 3 to 9 of the change: `FixedVec2`/`FixedVec3`/`FixedQuat`/`FixedTransform` and the shapes,
`RandomStream::unit_fixed()`, the `float-on-cross-platform-path` lint rule, the fixed-point movement
scenario and its cross-leg digest, networking's simulation identity and replay's kernel version, the
fixed-point mover and navigation, gameplay command validation, and the ABI and Swift types.
