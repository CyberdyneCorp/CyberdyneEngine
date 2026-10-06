# `src/core/detmath/` — deterministic math

Layer 0, target `cy::core-detmath`. Optional: nothing links it by default. Governed by
`deterministic-math` and by `simulation-and-determinism`'s floating-point policy. Built by stages 1
and 2 of `openspec/changes/add-deterministic-math`.

Authoritative arithmetic whose result does not depend on which machine ran it. Every operation is
built from integer instructions, which C++20 defines exactly; the transcendentals are polynomials
whose coefficients are generated integers. Nothing comes from a C library, and nothing depends on a
compiler's floating-point choices.

| Header | What it is |
|---|---|
| `fixed.h` | `Fixed` (Q32.32 in an `i64`), `Fixed16` (Q16.16 storage), `Angle` (a `u32` binary angle), `WideFixed` (Q64.64), and their arithmetic |
| `functions.h` | `sqrt`, `sin`/`cos`/`sincos`/`tan`, `atan`/`atan2`/`asin`/`acos`, `exp2`/`log2`/`exp`/`log`/`pow`, each with a declared error bound |
| `convert.h` | The float boundary: `from_f32_cooked`, `from_f64_cooked`, `to_f64_relative`, `to_f32_relative` |
| `overflow.h` | `OverflowGuard`: wrapping is counted, and the first site named, in development builds |
| `version.h` | `kKernelVersion` |
| `digest.h` | The kernel digest every CI leg reproduces |
| `wide.h` | The 128-bit multiply and divide, and the three paths that implement them |
| `generated/coefficients.h` | The polynomial coefficients, written by `tools/detmath/gen_coefficients.py` |

## What this module claims

- **The rules are the specification.** Every implementation produces the same bits:

  | Operation | Rule |
  |---|---|
  | `+`, `-`, unary `-` | Two's-complement wrapping, computed in `u64`. Never undefined behaviour. |
  | `*` | The exact 128-bit product, rounded to nearest with ties toward +∞, wrapped. |
  | `/` | `(a << 32) / b` truncated toward zero, wrapped. Division by zero gives `max()` for `a >= 0` and `min()` for `a < 0`. |
  | `>>` | Arithmetic (floor). |
  | Comparison | Of the raw integer. No NaN, no infinity, no negative zero. |
  | `Fixed` → `Fixed16` | Round to nearest, ties toward +∞; out of range saturates. |
  | `Angle` | `u32` wrapping. `Angle × Fixed` scales through 128 bits. |

- **Bit-identical across the 128-bit paths.** `unsigned __int128` on GCC and Clang, `_umul128` and
  `_udiv128` on MSVC x64, `__umulh` and a 32-bit-digit long division on MSVC arm64. A portable
  32-bit-limb reference is compiled everywhere, and `unit.detmath` holds every path to it.
- **The declared error bounds** (`functions.h`), asserted by `integration.detmath_vectors` against an
  mpmath oracle, with the worst error printed beside each bound:

  | Function | Declared bound | Worst measured (oracle, 2 000+ cases each) |
  |---|---|---|
  | `sqrt` | ≤ 0.5 ulp (correctly rounded) | 0.50 ulp |
  | `sin`, `cos` | ≤ 1 ulp | 0.50 ulp |
  | `tan` | ≤ 2 ulp where \|tan\| ≤ 1; relative ≤ 2^-30 above | 1.0 ulp; relative 2^-34.6 near the pole |
  | `atan`, `atan2` | ≤ 1 ulp of `Angle` | 0.50 ulp |
  | `asin`, `acos` | ≤ 2 ulp of `Angle` for \|x\| ≤ 1 − 2^-16; ≤ 2^-15 turn nearer ±1 | 0.56 ulp |
  | `exp2` | the larger of 1 ulp and relative 2^-36 | 0.50 ulp |
  | `log2` | ≤ 2 ulp | 0.50 ulp |
  | `exp` | the larger of 2 ulp and relative 2^-34 | 0.50 ulp |
  | `log` | ≤ 3 ulp | 0.50 ulp |
  | `pow` (x > 0) | the larger of 2 ulp and relative 2^-34 + \|y\| 2^-58 | 0.50 ulp |
  | `Angle::from_radians` | ≤ 0.75 ulp of `Angle` (1/(2π) is held to 2^-64) | 0.75 ulp |
  | `Angle::radians`, `signed_radians` | ≤ 0.5 + 2^-28 ulp (2π is held to 2^-60) | 0.50 ulp |

  The polynomials themselves are well inside: the generator keeps the first degree whose fit error
  is below 2^-44 (2^-46 for `exp2`), and records the fit and the evaluated error beside the
  coefficients.
- **Exact identities**, asserted with `==`: `sin(a + quarter) == cos(a)`, `sin(-a) == -sin(a)`,
  `atan2(-y, x) == -atan2(y, x)`, `exp2(n) == 2^n`, `log2(2^n) == n`.
- **The kernel contains no floating point.** Its translation units are compiled a second time with
  `-mgeneral-regs-only`; a `float` that reaches them fails the build. `convert.cpp` is the boundary,
  and the one file excluded.
- **One computation at two vector widths gives one answer.** The kernel is compiled a third time with
  `-mavx2`, and `integration.detmath_variants` compares every function's digest across the three
  builds in one process.
- **Overflow wraps in every build, and is counted in development builds.** `OverflowGuard` names the
  first site; in Shipping the check is not compiled.

## What it does not claim

- **That it replaces `core/math`.** Rendering, animation, effects and every non-authoritative path
  keep `f32`. `core-math`'s requirement says so twice.
- **That a session using it is cross-platform deterministic.** Linking the module makes
  `DeterminismConfiguration::require()` stop refusing `CrossPlatform` and `Lockstep` as a whole (it
  exports `CY_DETERMINISM_MATH=1` as PUBLIC); each authoritative subsystem must then declare the
  profile itself, and the refusal names the first that does not. Today that is every float-based
  subsystem: physics, abilities, AI utility, root motion. Vector types, the movement step, navigation
  and the networking scope are later stages of the change.
- **Platforms with no CI leg.** The cross-leg comparison covers linux-x86_64, linux-arm64,
  macos-arm64 and windows-x86_64. macOS x86-64, Windows arm64, iOS and Android are not covered.
- **MSVC's float-free build.** MSVC has no `-mgeneral-regs-only`; the configure output says so on that
  leg rather than skipping in silence.

## Performance

`benchmarks/detmath/` measures one dependent chain per operation against design §11's budgets, with
thresholds in `benchmarks/baseline.json`. First measurement, `profile` build, i9-12900K under load:

| Operation | Budget | Measured |
|---|---|---|
| `*` | ≤ 2× `f64` `*` | 2.00× (2.18 ns) |
| `/` | ≤ 30 ns | 9.2 ns |
| `sqrt` | ≤ 40 ns | 24.6 ns |
| `sin` | ≤ 25 ns | 19.7 ns |
| `atan2` | ≤ 50 ns | 30.0 ns |
| `exp2`, `log2` | ≤ 30 ns | 26.8, 26.0 ns |

## The conversion boundary

Float becomes `Fixed` once — at cook, at session configuration, or when the issuing peer creates a
command — through `from_f32_cooked` and `from_f64_cooked`, and never inside a tick. The audit is a
grep:

```sh
grep -rn "from_f32_cooked\|from_f64_cooked" src
```

`Fixed` becomes `f32` for presentation only, relative to the camera, through `to_f32_relative`, which
subtracts in `Fixed` (exact) before its one rounding.

## Regenerating

```sh
just generate-detmath           # coefficients, golden vectors, oracle (needs mpmath==1.3.0)
just generate-detmath --check   # what `just generate-check` runs
```

A regeneration that moves any output bumps `kKernelVersion` in the same change;
`integration.detmath_vectors` fails until it does. `CY_DETMATH_RECORD_GOLDEN=1` makes that suite
rewrite the vector files from the C++ build and fail the run, so new expectations are never accepted
inside a green job.

## How the checks are made to fail

Each mutation below was applied to this tree, the suites rebuilt and run, and the mutation reverted
(GCC 13, Development, linux-x86_64).

| Mutation | What went red |
|---|---|
| The tie rule in `Fixed operator*` changed to round half to even | `unit.detmath` ("multiplication rounds to nearest with ties toward +infinity", and `WideFixed::narrow` against `*`); `integration.detmath_vectors` (`mul`: 3 of 276 golden outputs, the tie edges). `determinism.cross_leg` stayed green: a random sweep almost never draws an exact tie, which is why the tie inputs are chosen edges. |
| The reference multiply replaced with a `double` product | The `-mgeneral-regs-only` build of the kernel fails to compile (`SSE register return with SSE disabled`); built without it, `unit.detmath`'s native-against-reference cases go red. |
| The last bit of `kSin[0]` flipped | `integration.detmath_vectors` (`sin_core` 74 and `cos_core` 68 of 262 golden outputs, their digests, the kernel digest) and `determinism.cross_leg` (the committed kernel digest). The rounded `sin`/`cos` vectors did NOT move: a 2^-62 change rarely crosses a 2^-32 rounding boundary, which is why the Q2.62 cores have vectors of their own. |
| The last bit of `kAtan[0]` flipped | `integration.detmath_vectors` (`atan_core` 44 of 262, the kernel digest) and `determinism.cross_leg`. |
| The digest fold made order-insensitive (`accumulator + finalize(value)`) | `integration.detmath_vectors` (every function's digest) and `determinism.cross_leg`. |
| `CY_DETERMINISM_MATH=1` removed from the module's PUBLIC definitions | `unit.detmath` ("linking the module is what makes from_build() report deterministic math", and the acceptance case). |
| `CY_DETERMINISM_MATH=1` added to `unit.determinism`, which does not link the module | `unit.determinism` ("from_build() reports what this translation unit was compiled with"). |
| A disagreeing, zero, empty or missing `detmath-kernel-digest` | `tools/ci/test_cross_leg_digests.py`'s six `--detmath` fixtures: a finding (exit 1) or a refusal (exit 2), never an agreement. |
