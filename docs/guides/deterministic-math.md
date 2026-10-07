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
| [`src/core/detmath/`](../../src/core/detmath/README.md) | The module: `Fixed`, `Fixed16`, `Angle`, `WideFixed`, the vectors, rotations and shapes, the functions, the conversion boundary |
| [`src/movement/`](../../src/movement/README.md) | The fixed-point kinematic mover and the `Fixed` navigation world it moves units in |
| [`tools/detmath/`](../../tools/detmath/) | The coefficient generator, the Python model of the rules, the golden vectors and the oracle |
| [`benchmarks/detmath/`](../../benchmarks/detmath/bench_detmath.cpp), [`benchmarks/movement/`](../../benchmarks/movement/bench_movement.cpp) | The costs against the design's budgets |
| [`tests/determinism/test_cross_leg.cpp`](../../tests/determinism/test_cross_leg.cpp) | Publishes the kernel, movement and lockstep digests for the four-leg comparison |

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
7. [Authoritative movement under Lockstep](#7-authoritative-movement-under-lockstep)
8. [How it is proven](#8-how-it-is-proven)
9. [Performance](#9-performance)
10. [Changing the kernel](#10-changing-the-kernel)
11. [Pitfalls](#11-pitfalls)
12. [What is not built yet](#12-what-is-not-built-yet)

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

Built on those, `vec.h` and `shapes.h` give the geometry an authoritative system needs:

| Type | What it is |
|---|---|
| `FixedVec2`, `FixedVec3` | `Fixed` components. `dot`, `cross` (planar) and `length_squared` return an exact `WideFixed`; `length` and `distance` are correctly rounded; `normalize` truncates, and the zero vector is zero and counted |
| `Fixed16Vec3` | The 12-byte storage form, `widen()`/`narrow()` |
| `Rot2` | A planar rotation as its cosine and sine — an RTS heading |
| `FixedQuat`, `FixedTransform` | Rotation and placement, normalised after every composition; no `slerp` (interpolation is presentation) |
| `FixedAabb`, `FixedCircle`, `FixedCapsule2D` | Overlap and containment on exact squares: touching is not overlapping, on every peer |

`mul_div(a, b, c)` is `a * b / c` through the exact 128-bit product, truncated once: the scaling a
vector needs without the rounding of an intermediate `Fixed`. `RandomStream::unit_fixed_raw()` is the
raw `Fixed` in [0, 1) of a draw — its top 32 bits, exactly.

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
| `Angle::from_radians` | one product with 1/(2π) held to 2^-64 | 0.75 ulp of `Angle` (not correctly rounded) | 0.75 |
| `Angle::radians`, `signed_radians` | one product with 2π held to 2^-60 | 0.5 + 2^-28 ulp | 0.50 |

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

The module alone accepts nothing. Each subsystem declares what it guarantees:

| Subsystem | Declares |
|---|---|
| `movement` (the kinematic mover) | `Lockstep` |
| `navigation` | `Lockstep` when every authoritative world is `NavArithmetic::Fixed` with no runtime rebuilds, otherwise `SamePlatform` |
| `gameplay-commands` | `Lockstep` when the command stream runs under a cross-platform profile, so payloads are checked; otherwise `SamePlatform` |
| `physics` (Jolt authoritative), `abilities`, `ai-utility`, `root-motion` | `SamePlatform` |

```text
require(Lockstep)  movement + Fixed navigation + checked commands     ->  accepted
require(Lockstep)  ... + abilities, authoritative                     ->  SubsystemGuarantee, "abilities"
require(Lockstep)  ... with a Float navigation world                  ->  SubsystemGuarantee, "navigation"
```

The cases are `unit.determinism`'s three "CrossPlatform is ..." cases, `unit.detmath`'s two profile
cases, and `unit.movement`'s profile cases, which assemble the session above with the real
`from_build()`.

## 7. Authoritative movement under Lockstep

Physics stays presentation under `Lockstep` (design §8). Authoritative units move with the
fixed-point kinematic mover in [`src/movement/`](../../src/movement/README.md):

1. **The world is converted once.** The baked navigation mesh becomes a `FixedNavMesh` at load
   (`from_f32_cooked` on every vertex, before tick 0); terrain heights are cooked to `Fixed16`. A
   runtime rebuild of the source mesh is refused as authoritative input; dynamic obstacles are
   polygon flags set through commands.
2. **Orders arrive as commands with `Fixed` payloads.** Under `CrossPlatform` and `Lockstep`,
   `CommandStream::declare` refuses a declaration whose payload is undescribed or has a
   floating-point field, and `last_payload_refusal()` names the type and the field:

   ```cpp
   constexpr gameplay::PayloadField kMoveOrderFields[] = {
       {"squad", gameplay::PayloadFieldKind::Integer},
       {"target_x", gameplay::PayloadFieldKind::Fixed},
       {"target_z", gameplay::PayloadFieldKind::Fixed},
   };
   (void)commands.set_determinism_profile(determinism::DeterminismProfile::Lockstep);
   declaration.payload = gameplay::PayloadLayout{"MoveOrder", kMoveOrderFields, 3};
   ```

   The issuing peer converts its float pick (a camera ray hit) with `from_f32_cooked` when it creates
   the command; every other peer reads the raw value from the log.
3. **Paths are `Fixed`.** A* with `Fixed` g-costs, ties by polygon index; the funnel on exact
   cross-product signs; a `FixedFlowField` when many units share a destination.
4. **The mover steps.** Integration, pairwise separation over a `Fixed` grid, static circles and
   capsules, the clamp to the navigation surface, heights, headings — in entity order, with the
   same bits on any number of job workers.
5. **The scene is derived.** `publish_units` writes each unit's `AuthoritativeTransform`
   (`FixedTransform`, the hashed state); `sync_presentation` converts it camera-relative into the
   node's `LocalTransform` once per tick, and interpolation does the rest.

`src/movement/tests/rts_scenario.h` is the whole loop as a game wires it: squads ordered around walls
by four participants through a `Lockstep` command stream. Two peers in one process, the second
driven by the first's command log alone, agree on every tick (`integration.movement_lockstep`), and
the second's digest is compared between the four CI legs.

## 8. How it is proven

| Evidence | Where | Fails when |
|---|---|---|
| The rules, case by case | `unit.detmath` | a tie, a truncation, a wrap or a saturation is wrong |
| The 128-bit paths against the 32-bit-limb reference | `unit.detmath`, and a 20 000-input sweep in `integration.detmath_vectors` | an intrinsic disagrees on the leg that uses it |
| Golden vectors and per-function digests | `integration.detmath_vectors`, on **each leg alone** | this leg's C++ disagrees with the Python model of the rules |
| Declared bounds against mpmath | `integration.detmath_vectors` | a function leaves its bound |
| No float in the kernel | the `-mgeneral-regs-only` build in `integration.detmath_variants` | a float reaches a kernel source (the build fails) |
| Two vector widths, one answer | the `-mavx2` build in `integration.detmath_variants` | any function's digest differs |
| Four legs, one answer | `determinism.cross_leg` publishes `detmath-kernel-digest`; `cross-leg-compare` runs `--detmath` | two architectures disagree |
| A fixed-point simulation, four legs | `determinism.cross_leg` publishes `detmath-movement-digest` (2 000 units, 600 ticks) and the lockstep follower's `detmath-lockstep-digest`, each checked against a committed value on the leg first | a leg's movement moved, or two architectures disagree |
| Two peers, one command log | `integration.movement_lockstep` and `determinism.cross_leg`: the follower agrees with the issuer on every tick; a follower missing one command does not | a peer reads anything but the log |

The golden vectors and digests come from `tools/detmath/model.py`, the rules written a second time
in Python integers; the oracle comes from mpmath and shares no code with either. Run them:

```sh
just test-unit -R detmath
just test-integration -R detmath
just test-determinism -R cross_leg
just test-determinism --compare-legs --pcg --detmath --digests cross-leg-digests
```

## 9. Performance

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

`benchmarks/movement/` measures the movement step at strategy scale, 100 000 units (design §11:
at most 4 ms per tick on 8 workers, and at most 2.5× the same kernel in `f32`):

| Benchmark | What | First measurement |
|---|---|---|
| `movement/kernel-f32` | the crowd kernel — integration, the grid, separation — over an `f32` policy | MOVEMENT_KERNEL_F32 |
| `movement/kernel-fixed` | the same kernel text over `FixedPolicy` | MOVEMENT_KERNEL_FIXED |
| `movement/step` | the whole authoritative tick, one thread | MOVEMENT_STEP |
| `movement/step-8-workers` | the same tick on eight job workers | MOVEMENT_STEP_8 |

The measured figures and the committed thresholds are in
[`benchmarks/baseline.json`](../../benchmarks/baseline.json).

## 10. Changing the kernel

Any change that moves any output for any input — a coefficient, a reduction, a rounding rule —
bumps `detmath::kKernelVersion` (`version.h`) in the same change. The kernel version is folded into
the digest and published by every leg; later stages record it in replays and lockstep scopes so a
mismatch is a refusal at join, not a desync.

```sh
just generate-detmath            # coefficients, vectors, oracle (needs mpmath==1.3.0)
just generate-detmath --check    # what `just generate-check` runs in CI
CY_DETMATH_RECORD_GOLDEN=1 just test-integration -R detmath_vectors   # rewrites, and FAILS
```

## 11. Pitfalls

- **Squaring a distance in `Fixed`.** `d * d` overflows beyond 46 341 m. Use `WideFixed::product`.
- **Dividing by a value that may be zero.** It does not trap; it saturates and is counted. Test the
  divisor, or multiply by a reciprocal computed once.
- **Converting per tick.** A `from_f32_cooked` inside a system is a float on the authoritative path.
  It is legal C++ and it defeats the module.
- **Comparing angles with `<`.** `Angle` has no order; compare `signed_turns()` of a difference.
- **Swift's `+`.** It traps on overflow; the engine's rule is wrapping. The Swift `Fixed` (a later
  stage) uses `&+`.
- **A float field in a command payload.** It is read by every peer's own float hardware. Under a
  cross-platform profile the declaration is refused; carry the raw `Fixed` the issuer converted.
- **Adding units out of entity order.** The mover refuses it: its passes rely on unit order being
  entity order.
- **Trusting the compiler at its highest setting.** GCC 13 at `-O3` with LTO, the `release`
  profile, miscompiled the digest sweep. It unswitched the loop over the `KernelFunction` switch and
  sent `add` to the `default` branch, which folded 0 for every input. Sanitizers found nothing,
  and GCC 12, Clang and GCC 13 without LTO were correct. The sweep now takes the function as a
  template argument (`src/digest.cpp`), and the `profiles` job runs the vectors in `release`. The
  committed digests are why this showed up as a failure rather than as a new answer.

## 12. What is not built yet

The remaining stages of the change: the `float-on-cross-platform-path` lint rule, networking's
simulation identity and replay's kernel version, `Crowd`'s sampled reciprocal-velocity steering and
off-mesh links in a `Fixed` world, the ABI and Swift types (stage 8), the Jolt measurement and the
strategy-scale `Lockstep` scenario (stage 9).
