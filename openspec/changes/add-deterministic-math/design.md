# Design: deterministic math

## 1. Context

### What exists

**The `SamePlatform` half is built and measured.** The M9 spike
(`openspec/changes/archive/2026-09-11-implement-m9-integrity/design.md` §1) found two things:

- Floating-point contraction on a target with FMA moves the state hash. This is why
  `cy_declare_determinism_profile()` refuses a module compiled without `-ffp-contract=off`.
- Thirteen `<cmath>` functions are not correctly rounded in glibc. They are forbidden on
  authoritative paths and replaced by `cy::determinism::fp`.

`src/core/determinism/include/cy/core/determinism/fp_policy.h` says plainly what that replacement is
not:

> The correctly-rounded set is correctly rounded *in glibc 2.39 on x86-64*; a different libm may
> round `exp` differently and every function here would move with it. `CrossPlatform` and `Lockstep`
> need deterministic math types — fixed-point scalars and polynomial approximations that compute
> their own answers — and this tree has none.

**The refusals are in place and tested:**

| Where | What it refuses today |
|---|---|
| `DeterminismConfiguration::require()` | Every `CrossPlatform` and `Lockstep` session, with `ProfileRefusal::DeterministicMathMissing`. It reads `BuildConfiguration::deterministic_math_available`, which comes from the `CY_DETERMINISM_MATH` macro. Nothing defines that macro. |
| `cy::networking::profile_required_by(NetworkMode::Lockstep)` | Nothing directly: it asks only for `SamePlatform`. `join_verdict()` then refuses a lockstep peer that differs in platform, architecture or build (`JoinRefusal::PlatformMismatch`, `ArchitectureMismatch`, `BuildMismatch`). |
| `cy::physics::validate_session()` | `SessionDeterminism::CrossPlatform` and `Lockstep` with `PhysicsAuthority::Authoritative`. |

**The cross-architecture comparison exists.** `.github/workflows/ci.yml`'s `cross-leg-publish` runs
`just test-determinism --publish-digest` on linux-x86_64, linux-arm64, macos-arm64 and
windows-x86_64. `cross-leg-compare` runs `tools/ci/cross_leg_digests.py`, which refuses to report
agreement in any of these cases:

- only one leg published;
- every leg has the same architecture;
- a digest is zero;
- a workload is empty.

`tests/determinism/README.md` records that the linux arm64 and x86-64 digests of the toy session
agree. That session is float code. So the agreement is evidence that the engine's own float
primitives behave the same way on two architectures, for one workload. It is not a guarantee, and
this design does not treat it as one (§3.1).

### Constraints that shape every decision

- **The ABI rules** (`openspec/config.yaml`): no exceptions, no RTTI, fallible calls return
  `cy::Expected`, and the engine core never links the Swift runtime.
- **Layer 0 depends only on lower layers.** The module sits beside `cy::core-math` and
  `cy::core-determinism`.
- **`core-math` stays `f32`.** The requirement says so twice: "The deterministic math module SHALL
  NOT replace this library."
- **The project's checks must fail when they should.** Every claim below comes with the mutation
  that has to turn it red (§10).

## 2. Goals and non-goals

**Goals**

- Authoritative arithmetic whose results are bit-identical on every compiler, optimisation level,
  vector width and architecture the CI legs cover.
- Transcendentals with declared error bounds, verified against a high-precision oracle.
- A conversion boundary narrow enough to audit by grep.
- An authoritative movement path for `Lockstep` that does not depend on the physics backend.
- `CrossPlatform` and `Lockstep` accepted for sessions that meet them, and refused, with the
  subsystem named, for sessions that do not.

**Non-goals**

- Replacing `core/math`, or converting rendering, animation, VFX or audio.
- A fixed-point rigid-body solver (§8).
- Converting abilities attributes, AI utility scoring or animation root motion to fixed point.
  Under `Lockstep` those subsystems keep declaring `SamePlatform`, and the refusal names them.
  Converting them is follow-up work.
- Claims for platforms with no CI leg: macOS x86-64, Windows arm64, iOS, Android.

## 3. Decision: fixed point, not strict floating point, not soft-float

### 3.1 The alternatives

| Approach | How it is deterministic | Why it is or is not chosen |
|---|---|---|
| **Strict IEEE floats.** Contraction off, no fast-math, fixed denormal mode, own transcendentals. | IEEE 754 requires `+ - * / sqrt` to be correctly rounded, so the same operations in the same order give the same bits. Jolt's `CROSS_PLATFORM_DETERMINISTIC` mode works this way. | **Rejected as the cross-platform mechanism.** It is kept as the `SamePlatform` mechanism, which is what it already is. Its correctness depends on how every translation unit on the path is compiled, by every compiler, including the Swift compiler for gameplay code and the compilers that build plugins. The engine cannot enforce that from the outside. When it fails, the failure is a desync hours later, not a compile error. |
| **Soft-float.** IEEE arithmetic emulated in integers. | Integer arithmetic. | **Rejected.** It is typically one to two orders of magnitude slower than hardware floats, and the strategy-scale benchmark (100 000 units at 60 Hz) cannot absorb that. It also keeps float semantics, with NaN, infinity and signed zero, that authoritative code does not want. |
| **Fixed point.** | Integer arithmetic, which C++20 defines exactly, including two's-complement conversion. The transcendentals are written in the same arithmetic, so no part of the result comes from a C library or from a compiler's choices. | **Chosen.** If a value has type `Fixed`, it was computed deterministically. That is checked by the type system, by a compile flag and by the lint (§7). It is not a convention. |

### 3.2 Why strict floats are not enough

Each of these is a way the same source can produce different bits. Each is controllable for
engine code and none is controllable for all code on an authoritative path:

- **Contraction.** arm64 always has FMA, so `__FP_FAST_FMA` is defined there and the M9 spike's
  `FloatContraction` refusal applies on every arm64 leg. GCC's default is `-ffp-contract=fast`.
  MSVC's default model is not the same model as Clang's or GCC's.
- **Transcendentals.** glibc, Apple's libm and the MSVC runtime are different implementations. The
  `fp::` replacements are built on glibc's correctly-rounded set, so they move with the libm.
- **Denormal state.** Flush-to-zero and denormals-are-zero are per-thread state. Any library that
  sets them on a worker, for example audio DSP, changes results for every job that later runs on
  that worker.
- **Swift.** Gameplay code is compiled by `swiftc`, and the engine has no compile-flag contract over
  it. `docs/guides/swift.md`'s determinism rules are rules the guide says the engine "cannot"
  enforce.
- **Compiler upgrades.** A new optimiser may reassociate or vectorise differently and still be
  conforming.

Fixed point removes every item on this list, because none of them changes what an integer
instruction computes.

### 3.3 The cost, stated

- Fixed point has a range. Overflow is a real failure mode (§4.3).
- It is slower for division and transcendentals (§11).
- It needs its own vector types. Code that wants cross-platform determinism has to use them.
  `simulation-and-determinism` already calls this "a stated cost rather than an assumption".

## 4. Decision: formats, chosen per use

### 4.1 The formats

| Type | Storage | Format | Range | Resolution | Used for |
|---|---|---|---|---|---|
| `Fixed` | `i64` | Q32.32 | ±2 147 483 648 | 2^-32 ≈ 2.3×10^-10 | **All authoritative arithmetic.** Positions, velocities, times, distances, costs, magnitudes. |
| `Fixed16` | `i32` | Q16.16 | ±32 768 | 2^-16 ≈ 1.5×10^-5 | **Storage only.** Dense per-element data where the range and resolution suffice. Widened to `Fixed` on load; never used for arithmetic. |
| `Angle` | `u32` | binary angle, a full turn = 2^32 | one turn, wrapping | 2^-32 turn ≈ 1.46×10^-9 rad | Headings, rotation inputs to `sin`/`cos`, results of `atan2`. Wrapping is free and exact. |
| `WideFixed` | two `u64` limbs, or `__int128` where available | Q64.64 | ±9.2×10^18 | 2^-64 | Intermediate only. Products, dot products and squared lengths, so that comparing two distances cannot overflow. |
| Kernel format | `i64` | Q2.62 | ±2 | 2^-62 | Internal to the transcendental kernels: reduced arguments and polynomial evaluation. Not public. |

Units do not change: metres, seconds, kilograms and radians, as in `core-math`. `Angle` is the one
deliberate exception, because a binary angle makes range reduction exact.

### 4.2 Why these and not others

- **Q16.16 as the arithmetic type is rejected.** `x * x` overflows at |x| > 181. That makes a squared
  distance overflow beyond 181 m, which an RTS map exceeds immediately.
- **Q48.16 as the only type is rejected.** It is a common choice for lockstep engines because 64-bit
  storage with 16 fractional bits leaves a large range. Its resolution of 1.5×10^-5 is coarse for
  two things the engine needs:
  - Slow accelerations. Gravity per tick at 60 Hz is 0.0027 m/tick², quantised to about 0.6 %.
  - Transcendentals. `sin` cannot be more accurate than its output format.
- **Q40.24 is rejected.** It still needs a 128-bit product to be safe. It also loses Q32.32's
  alignment: there the binary point falls on a 32-bit limb boundary, so the portable limb multiply
  needs no shift across limbs, and every `f32` with |v| < 2^31 and lowest set bit ≥ 2^-32 converts
  exactly.
- **Absolute positions, not cell-relative.** `core-math` stores persistent positions cell-relative
  because `f32` cannot hold a large world. Q32.32 covers about 2 million km at sub-nanometre
  resolution, so authoritative `Fixed` positions are world-absolute. Camera-relative conversion for
  rendering is then one exact subtraction (§7.2). Cell membership is still derived for streaming and
  for relevance.
- **`Fixed16` exists for storage density.** `core-math`'s rule that `Vec3` is 12 bytes and never
  padded applies to large populations. `FixedVec3` is 24 bytes, which is 2.4 MB for 100 000 units. A
  project can store a component as `Fixed16Vec3` (12 bytes) when the range fits, and pay a widening
  load.

### 4.3 Arithmetic rules

These rules are the specification. Any implementation, whether an intrinsic, the portable limbs or
Swift, must produce the same bits.

| Operation | Rule |
|---|---|
| `+`, `-`, unary `-` | Two's-complement wrapping, computed in `u64` and converted back. Signed overflow is never undefined behaviour in this module. |
| `*` | The exact 128-bit product, rounded to nearest with ties toward +∞ (add 2^31, then an arithmetic shift right by 32), then wrapped to 64 bits. |
| `/` | The exact quotient of `(a << 32) / b` in 128 bits, truncated toward zero, then wrapped. Division by zero returns `Fixed::max()` for `a ≥ 0` and `Fixed::min()` for `a < 0`, in every build. |
| Shifts | Arithmetic right shift (floor), as C++20 defines it for signed values. |
| Comparison | Integer comparison of the raw value. There is no NaN, no infinity and no negative zero, so every comparison is total. |
| Conversion `Fixed` → `Fixed16` | Round to nearest, ties toward +∞. Out of range saturates and is counted. |
| `Angle` arithmetic | `u32` wrapping. `Angle` × `Fixed` scales through 128 bits and wraps. |

**Overflow wraps rather than saturates, and development builds count it.**

- *Saturation was rejected.* It costs a branch on every operation. It also turns a bug into a
  plausible wrong value: a unit stuck at the map edge looks like gameplay.
- *Trapping was rejected.* It would make shipping and development builds compute different things.

Wrapping is deterministic, so peers that overflow still agree. A `detmath::OverflowGuard`, modelled
on `cy::determinism::NonFiniteGuard`, counts overflows and names the first site in development
builds, and compiles to nothing in Shipping. Explicit `saturating_add` and `saturating_mul` exist for
gameplay clamps that want saturation.

### 4.4 The 128-bit product and quotient

| Toolchain | Multiply | Divide |
|---|---|---|
| GCC and Clang, 64-bit | `__int128` | `__int128` (the compiler's `__divti3`) |
| MSVC x64 | `_mul128` | `_div128` |
| MSVC arm64 | `__mulh` and the low product | the portable long division |
| Every toolchain | the **portable reference**: 32-bit limbs, the way `cy::detail::mix` in `src/core/memory/include/cy/core/memory/hash.h` already computes a 64×64→128 product on MSVC | same |

The portable reference is compiled on every leg. The unit suite checks that the selected path matches
it, bit for bit, over the golden vectors and a random sweep. An intrinsic that disagrees fails on the
leg that uses it.

## 5. Decision: transcendentals are polynomials, generated, with declared bounds

### 5.1 Table or polynomial or CORDIC

| Approach | Accuracy at a reasonable cost | Why chosen or not |
|---|---|---|
| Lookup table with linear interpolation | A 4096-entry quarter-wave sine table interpolated linearly has error ≈ (π/2/4096)²/8 ≈ 2^-25.7. | **Rejected for trigonometry.** 2^-25.7 is about 75 ulp of Q32.32. Getting closer needs larger tables or cubic interpolation. Every worker shares cache with the simulation's own data, and the table is one more artefact that must be byte-identical on every peer. |
| CORDIC | One bit per iteration, using only shifts and adds. | **Rejected.** It needs 32 or more dependent iterations for Q32.32. A polynomial on a 64-bit hardware multiply is several times faster. |
| **Range reduction, then a fixed-point polynomial** | Chebyshev-fit polynomials on the reduced intervals give the truncation errors in §5.3. Rounding adds about one 2^-62 step per Horner step in the kernel format. | **Chosen.** The coefficients are integer constants. Their values *are* the definition of the function, they are generated by a committed script, and they are reviewed like any other constant. |

### 5.2 The reductions

| Function | Input → output | Reduction |
|---|---|---|
| `sqrt` | `Fixed` (≥ 0) → `Fixed` | None. Integer square root of the 128-bit value `raw << 32`, rounded to nearest. **Correctly rounded.** A negative input returns 0 and is counted. |
| `rsqrt`, `length`, `distance`, `normalize` | through `WideFixed` | The integer square root of the 128-bit sum, so a squared length never materialises as a `Fixed`. |
| `sin`, `cos`, `sincos`, `tan` | `Angle` → `Fixed` | The top 3 bits select the octant (exact, because the angle is binary). The rest becomes a Q2.62 argument in [0, π/4]. A sine or cosine polynomial follows, with octant symmetry. |
| `atan`, `atan2` | `Fixed` → `Angle` | Octant from the signs and the comparison of \|y\| with \|x\|. The ratio min/max is taken in Q2.62. A second reduction about tan(π/8) brings the argument to [0, tan(π/8)], followed by a polynomial. `atan2(0, 0)` returns 0. |
| `asin`, `acos` | `Fixed` → `Angle` | `atan2(x, sqrt(1 − x²))`, with the square root computed through `WideFixed`. An input outside [−1, 1] is clamped and counted. |
| `exp2` | `Fixed` → `Fixed` | The integer part becomes a shift. The fractional part in [0, 1) goes to a polynomial. A result beyond range saturates and is counted. |
| `log2` | `Fixed` (> 0) → `Fixed` | `countl_zero` (C++20 `<bit>`, exact) normalises to m ∈ [1/√2, √2). Then s = (m − 1)/(m + 1) and an odd series in s. |
| `exp`, `log`, `pow` | built from the two above | Multiplication by log2(e) or ln 2 as Q2.62 constants. `pow(x, y)` = `exp2(y · log2(x))` is offered for x > 0 only. |

### 5.3 Error bounds

The bounds are against the exact real function **of the quantised input**. A function cannot be
more accurate than its input. "ulp" means one unit of the output format: 2^-32 for `Fixed`, and
2^-32 turn for `Angle`.

| Function | Polynomial (estimate) | Truncation error of the fit | Declared bound |
|---|---|---|---|
| `sqrt` | none | 0 | ≤ 0.5 ulp (correctly rounded) |
| `sin`, `cos` | degree 9 on [0, π/4] | ≈ 2^-43 | ≤ 1 ulp |
| `tan` | `sin`/`cos` quotient | | ≤ 2 ulp for \|result\| ≤ 1; relative ≤ 2^-30 above that |
| `atan`, `atan2` | degree 11 on [0, tan(π/8)] | ≈ 2^-41 | ≤ 1 ulp of `Angle` |
| `asin`, `acos` | through `atan2` | | ≤ 2 ulp of `Angle` for \|x\| ≤ 1 − 2^-16; ≤ 2^-15 turn closer to ±1, where the function's own slope dominates |
| `exp2` | degree 9 on [0, 1) | ≈ 2^-44 (relative) | the larger of 1 ulp and a relative error of 2^-36 |
| `log2` | odd series on the reduced m | ≈ 2^-40 | ≤ 2 ulp |
| `exp`, `log` | composed | | the larger of 2 ulp and a relative error of 2^-34 (`exp`); ≤ 3 ulp (`log`) |

The degrees and fit errors are estimates from Chebyshev fits made while writing this design. Stage 2
fixes the final degrees: `tools/detmath/gen_coefficients.py` chooses each polynomial and writes its
measured error into the generated header beside the coefficients. The property tests (§10.3) then
assert the **declared** bound, not the estimate. If a function cannot meet its declared bound, the
bound changes in the same change, with the reason recorded, and is never silently loosened.

### 5.4 Versioning

`detmath::kKernelVersion` is bumped whenever any function's output changes for any input:

- a coefficient;
- a reduction;
- a rounding rule.

It works the way `cy::determinism::kMixerVersion` works for random draws. It is recorded in replay
manifests, in lockstep compatibility scopes and in the published cross-leg digest. So a mismatch
between peers is reported as a version mismatch at join or load, not as a desync.

## 6. Decision: vector, quaternion and transform types

| Type | Components | Notes |
|---|---|---|
| `FixedVec2`, `FixedVec3` | `Fixed` | `dot` and `length_squared` return `WideFixed`. `length`, `distance` and `normalize` use the wide square root. `normalize` of the zero vector returns zero and is counted. |
| `Fixed16Vec3` | `Fixed16` | Storage form only. Explicit `widen()` and `narrow()`. |
| `Rot2` | `Fixed` cosine and sine | Planar rotation built from an `Angle`. This is what an RTS heading uses, rather than a quaternion. |
| `FixedQuat` | `Fixed` | Composition, rotation of a vector, and construction from an axis and an `Angle`. `normalize()` goes through the wide square root and is applied after every composition. Equality is raw equality. There is no `slerp`: interpolation is presentation work and happens in `f32` after conversion. |
| `FixedTransform` | `FixedVec3` translation and `FixedQuat` rotation | No scale. Authoritative simulation does not scale units. A project that needs it adds a `Fixed` uniform scale to its own component. |
| `FixedAabb`, `FixedCircle`, `FixedCapsule2D` | | The shapes the kinematic mover (§8) and navigation (§9) need. Overlap and sweep tests use `WideFixed` products. |

There is no `FixedMat4`. Projection and skinning matrices are rendering concerns. Rotation matrices
are derived from `FixedQuat` only where a kernel needs one, and never stored.

The types follow `core-math`'s conventions: right-handed, Y-up, −Z forward, rotation
counter-clockwise about an axis seen from its positive end. `tests/test_conventions.cpp` in the new
module asserts the same numeric consequences that `src/core/math/tests/test_conventions.cpp` asserts
for `Vec3` and `Quat`. Both libraries must name the same direction "forward".

## 7. Decision: the conversion boundary

### 7.1 Float into `Fixed`: once, recorded, never per tick

| Where | Permitted conversion | Why it is deterministic |
|---|---|---|
| **Cook** | `Fixed::from_f32_cooked` and `from_f64_cooked`. The cooked asset stores the raw `i64`. | It runs once. Every peer loads the same bytes, because the content manifest hash is part of the compatibility scope. |
| **Session configuration** | The same functions, applied to configuration values before tick 0. The results are folded into the session's state hash. | Peers that disagree are detected at tick 0, not later. |
| **Command creation** | The issuing peer converts a float pick (a camera ray hit, a drag) into `Fixed` and puts the raw value in the command payload. | Only the issuer converts. The command log carries the result, so every other peer, and every replay, reads the same bits. This is `gameplay-framework`'s "one command stream" applied to numbers. |
| **Anywhere in a tick** | **None.** | |

The `from_*_cooked` functions multiply by 2^32 (exact) and round with `std::nearbyint` in
round-to-nearest mode. `nearbyint` is in `fp_policy.h`'s exact-by-IEEE set, so the conversion is
itself deterministic across platforms. Even so, it is restricted to the three moments above, so that
the authoritative tick never contains a float.

### 7.2 `Fixed` into `f32`: presentation only, relative to the camera

`to_f32_relative(FixedVec3 value, FixedVec3 origin)` does three things:

1. It subtracts in `Fixed`. This is exact unless the difference overflows, and a camera more than
   2×10^9 m from a unit is not a case to design for.
2. It converts the difference to `f64`. This is exact for differences within ±2^21 m, and correctly
   rounded beyond that.
3. It rounds once to `f32`.

The result is camera-relative, so `core-math`'s large-world rule (camera-relative rendering) costs
nothing extra.

That `f32` value is presentation-classified. A presentation-sync system writes it into the
interpolation inputs that `scene::InterpolatedTransform` already holds as `Presentation` fields.
`Classified<>` makes a read back into an authoritative system impossible to write
(`src/core/determinism/README.md`, claim 1).

### 7.3 Enforcing it

- **The kernel cannot contain a float.** The module's kernel translation units are compiled a second
  time, in the unit-test target, with `-mgeneral-regs-only` where the compiler accepts it. This is
  probed with `check_cxx_compiler_flag`, as `src/core/math/CMakeLists.txt` probes `-mavx2`. Any
  float or double in a kernel then fails to compile. Conversions live in a separate translation unit,
  `convert.cpp`, which is excluded from that check.
- **The lint.** `determinism_lint.py` gains a rule, `float-on-cross-platform-path`. It reports
  `float`, `double`, `f32`, `f64` and the `core-math` vector types in the sources of a target whose
  `cy_declare_determinism_profile()` declaration is `CrossPlatform` or `Lockstep`. It gets a
  `SELFTEST_CASES` entry like the other rules. This closes `simulation-and-determinism`'s "use of
  floating-point operations disallowed by the active profile" for the declared targets.
- **The audit.** `grep -rn "from_f32_cooked\|from_f64_cooked"` lists every entry point. Review keeps
  that list to cook code, session configuration and command creation, which is the same treatment
  `bypass_classification()` gets.

## 8. Decision: physics under `Lockstep`

| Option | What it means | Verdict |
|---|---|---|
| **(a) Jolt in `CROSS_PLATFORM_DETERMINISTIC` mode, authoritative** | `cmake/dependencies.cmake` already turns the switch on. Jolt documents cross-platform determinism under conditions: the same Jolt version, the same order of operations, and its own floating-point controls. | **Not claimed now.** This tree has measured only the same-platform half (`src/servers/physics/include/cy/servers/physics/determinism.h`: "NOT CLAIMED, AND NOT TESTED: anything across platforms"). Stage 9 adds a Jolt scene's per-tick hash to the cross-leg digests **as a measurement**. The engine claims nothing until that measurement has agreed on all four legs over a soak set, and even then the claim takes its own change, which would add a `DeterminismPolicy` enumerator. This keeps the door open without assuming the answer. |
| **(b) A fixed-point rigid-body solver** | Rewrite contacts, constraints and islands in `Fixed`. | **Rejected.** It contradicts the locked decision to integrate Jolt rather than rebuild it. It is years of work, and a lockstep RTS rarely needs authoritative rigid bodies. |
| **(c) A fixed-point kinematic mover; physics `NonAuthoritative`** | Authoritative units move with a `Fixed` integrator. Collision is against static navigation and height data and between units, using circles and capsules on the plane. Jolt runs debris, ragdolls and secondary effects as presentation. | **Chosen.** This is already the escape hatch that `physics` and `validate_session()` name: `PhysicsAuthority::Presentation`. |

The mover is not a physics engine. It integrates velocity, resolves overlap by deterministic
pairwise separation, and clamps to the navigation surface. Its neighbour search uses a grid keyed by
`Fixed` cell coordinates. It iterates in entity order, as `simulation-and-determinism`'s
tie-breaking rules require. Height comes from heightfield samples converted at cook and stored as
`Fixed16`, so the terrain's float data is never read per tick.

## 9. Decision: navigation and gameplay under `Lockstep`

### 9.1 Navigation

- **The baked navmesh is content.** Recast bakes in float, offline. The `.cynavmesh` bytes are
  identical on every peer because the content manifest hash matches. At load, a `Lockstep` world
  converts vertices to `FixedVec3` with the cooked conversion (§7.1). That happens once, before
  tick 0.
- **Queries.** A* in the `Lockstep` world uses `Fixed` g-costs. The Euclidean heuristic uses the wide
  square root, and ties break by polygon id, as today. The funnel uses `WideFixed` cross-product
  signs, so collinear and near-collinear cases resolve the same way everywhere. Flow-field
  integration costs become `Fixed`. Its queue already breaks ties by cell index
  (`src/navigation/include/cy/navigation/flow_field.h`).
- **Crowd.** `Crowd`'s ORCA linear programs (`crowd.h`) run in `Fixed`, with `WideFixed` dot
  products. The algorithm is written once, over a scalar policy, and instantiated for `f32` and for
  `Fixed`. This gives one algorithm text with two arithmetic instantiations, instead of two copies
  that would drift apart. The cost is template complexity in `crowd.cpp`, which we accept.
- **Runtime navmesh rebuild is refused as an authoritative input under `Lockstep`.** A Recast tile
  rebuild at run time is float work on each peer. Dynamic obstacles in a `Lockstep` world use
  integer per-polygon flags and flow-field cost overlays instead. A world that needs runtime
  rebuilds declares navigation `SamePlatform`, and the refusal names it.
- **Selection.** Each navigation world declares `NavArithmetic::Float` or `NavArithmetic::Fixed`.
  The navigation subsystem declares `CrossPlatform` only when every authoritative world is `Fixed`.

### 9.2 Gameplay

- **Commands.** A command payload is 48 bytes (`kMaxCommandPayload` in
  `src/gameplay/include/cy/gameplay/command.h`), which is enough for a `FixedVec3` (24 bytes) and
  more. `Command::set_payload` already copies trivially copyable values. No code change is needed
  for payloads to carry `Fixed` values. What changes is the rule: under a cross-platform profile, a
  command type whose payload contains `f32` or `f64` fails registration validation, with the type
  named.
- **Authoritative state.** An `AuthoritativeTransform` component (`FixedTransform`) is the state that
  is hashed and snapshotted. A presentation-sync system, once per tick, writes the scene transform
  and its interpolation pair from it. The scene transform stays `f32` and becomes derived state.
- **Random.** `RandomStream` gains `unit_fixed()`, which returns a `Fixed` in [0, 1) from the top 32
  bits of a draw. This is exact, like `unit_float()`.
- **Hashing.** `Fixed` is an `i64`, so `StateHashTree::mix_i64` and the generated codecs' direct
  encoding apply unchanged. No new hash or codec path is needed, which keeps the hash definition
  stable.
- **What stays float, and is named.** These subsystems keep declaring `SamePlatform` until a later
  change converts them, and a `Lockstep` session that uses them authoritatively is refused naming
  them:
  - abilities attributes (`f32` in `src/gameplay/abilities/include/cy/gameplay/abilities/attributes.h`);
  - AI utility scores;
  - root motion.

## 10. Decision: proving it across architectures

Five independent pieces of evidence. Each can fail on its own.

### 10.1 Golden vectors, checked on every leg alone

The vector files live under `tools/detmath/vectors/`. They hold inputs and the expected raw outputs
for every public function:

- the edge cases: zero, ±1 ulp, the extremes of range, octant boundaries, `atan2` on the axes, and
  overflow boundaries;
- about 10^5 seeded random inputs per function.

`unit.detmath` checks them **on each leg**. The answer is committed, so a single leg that diverges
fails by itself, without waiting for a comparison. Regeneration follows `determinism.golden_replay`:
`CY_DETMATH_RECORD_GOLDEN=1` rewrites the files **and fails the run**, so it cannot happen silently
inside a green job.

### 10.2 The kernel digest and the movement digest, compared between legs

`determinism.cross_leg` publishes two new fields:

- `detmath-kernel-digest`: every golden output folded in order;
- `detmath-movement-digest`: the per-tick state hash of a fixed-point movement scenario. The
  scenario is 2 000 units on a navmesh with crowd steering, separation and seeded random orders,
  running for 600 ticks.

`tools/ci/cross_leg_digests.py` gains a `detmath` entry in `CLAIMS`. It inherits every refusal the
comparator already has: one leg, one architecture, a zero digest, an empty workload. The comparison
covers whichever compilers build the four legs: GCC or Clang on Linux, Apple Clang on macOS and MSVC
on Windows.

### 10.3 Property tests against a high-precision oracle

`tools/detmath/gen_oracle.py` evaluates each function with arbitrary-precision arithmetic (mpmath) at
seeded inputs. It writes each reference as a 128-bit-scaled integer, so the C++ test needs no
high-precision library. `unit.detmath` asserts each function's declared bound (§5.3) over those
inputs and reports the worst error found next to the bound. It also asserts these properties:

- monotonicity of `sqrt`, `exp2`, `log2` and `atan` over sorted inputs;
- `sin² + cos²` within a stated tolerance;
- the identities that hold exactly in binary-angle form, such as `sin(a + quarter) == cos(a)` for
  every `a`.

### 10.4 Optimisation levels and vector widths, in one process

The kernel digest is computed by the module as built at each of the four build profiles. The AVX2
suite pattern in `src/core/math/CMakeLists.txt` adds a second build of the kernel with `-mavx2`. That
build is linked into one test and compared with the baseline build, digest for digest. This is the
case the `core-math` "Precision" exemption asks for: "one authoritative computation across two
vector widths with identical results". Integer SIMD added later must match the scalar reference bit
for bit. This is stricter than `core-math`'s "within documented tolerance", and integer SIMD can meet
it.

### 10.5 Falsification

Each mutation below must turn something red. Each is recorded with `just roadmap-falsify` in the
stage that adds the corresponding check.

| Mutation | What must go red |
|---|---|
| Flip the last bit of one `sin` coefficient | `unit.detmath` golden vectors on every leg |
| Replace the portable 128-bit multiply with a `double` product | the `-mgeneral-regs-only` build, and the golden vectors |
| Change the tie rule in `*` to round half to even | golden vectors at the tie inputs |
| Fold the movement hash with `+` instead of `hash_combine` | `determinism.cross_leg` (the same mutation its README already records for the simulation digest) |
| Publish a float movement digest under the `detmath` claim | the comparator, on the first leg that disagrees. This is not guaranteed to fail on any given run, so it is a demonstration, not a falsification proof, and it is labelled that way. |
| Remove a `f32` payload check from command registration | the gameplay case that registers a float payload type under `Lockstep` |

## 11. Performance budget

All figures are for the CI x86-64 reference runner, measured by `benchmarks/` cases that stage 1 and
stage 2 add with baselines in `benchmarks/baseline.json`. They are budgets to defend, not
predictions. Stage 1's first measurement either confirms them or changes them in a reviewed baseline
edit.

| Operation | Budget | Reasoning |
|---|---|---|
| `+`, `-`, compare | the same as `i64` | They are `i64` instructions. |
| `*` | ≤ 2× an `f64` multiply in a dependent chain | A 64×64→128 multiply, an add and a shift (x86-64 `imul`; arm64 `mul` and `smulh`). |
| `/` | ≤ 30 ns | A 128/64 division, through `__divti3`, `_div128` or limbs. Gameplay code should multiply by a reciprocal computed once where it can. |
| `sqrt` | ≤ 40 ns | An integer square root over 128 bits. |
| `sin`, `cos`, `sincos` | ≤ 25 ns | One reduction and a degree-9 Horner evaluation in Q2.62. |
| `atan2` | ≤ 50 ns | One division and a degree-11 Horner evaluation. |
| `exp2`, `log2` | ≤ 30 ns each | |
| Movement step | 100 000 units with separation ≤ 4 ms per tick on 8 workers, and ≤ 2.5× the same kernel in `f32` | This is the term that matters for the strategy-scale benchmark in `simulation-and-determinism`. |

## 12. Profile acceptance, networking scope and replay

### 12.1 The profile check

- `cy::core-detmath` exports `CY_DETERMINISM_MATH=1` as a **PUBLIC** compile definition, so
  `BuildConfiguration::from_build()` reports `deterministic_math_available` in any translation unit
  that links the module.
- `require()` keeps its order: build checks first, then subsystems in name order. With the module
  linked, `CrossPlatform` and `Lockstep` reach the subsystem loop, which refuses the first
  authoritative subsystem that declares less.
- The refusal reason `DeterministicMathMissing` stays, for builds without the module. The case
  `determinism: CrossPlatform is refused because this tree has no deterministic math` is rewritten
  into three cases:
  1. refused when the module is absent;
  2. accepted when the module is present and every subsystem declares `CrossPlatform`;
  3. refused, naming `physics`, when physics is authoritative.

### 12.2 Networking

`profile_required_by()` gains the session's declared profile as an input. A `Lockstep`-mode session
that declares `DeterminismProfile::Lockstep` requires that profile. One that declares `SamePlatform`
keeps today's behaviour, so **same-platform lockstep remains available** for projects whose
subsystems are not converted.

`CompatibilityScope` gains `simulation_identity`, a hash of four things:

- the project source revision;
- `detmath::kKernelVersion`;
- `determinism::kMixerVersion`;
- the content manifest hash.

`join_verdict()` compares platform, architecture and `build_id` only when the scope's profile is
below `CrossPlatform`. At or above it, `join_verdict()` compares `simulation_identity`,
`schema_set_hash` and the profile. A new enumerator, `JoinRefusal::SimulationIdentityMismatch`,
names a mismatch.

### 12.3 Replay

`replay::CompatibilityManifest` gains `detmath_kernel_version` and `simulation_identity`. For a
`CrossPlatform` or `Lockstep` replay, `Reproducible` requires the same simulation identity instead of
the same `engine_build`. A replay recorded on arm64 is then reproducible on x86-64, and that is the
point of the profile.

## 13. Swift exposure

- **Across the ABI, a `Fixed` is a raw `int64_t`.** One minor-version append, whichever minor is next
  when the stage lands (the header is at 1.4 today), adds:
  - `CyFixed` (`int64_t`), `CyAngle` (`uint32_t`), and `CyFixedVec3` and `CyFixedQuat` as POD
    structs of those;
  - `CY_VAR_FIXED` in `CyVarType`, with the `as_i64` payload, following the 1.1 rule that the tag
    names the storage width;
  - typed component accessors;
  - `detmath_*` entries for the transcendentals, with batch variants that take spans.

  Nothing existing changes, so the ABI gate accepts it as an append.
- **In Swift, arithmetic is inline and transcendentals call the engine.**
  - `CyberdyneKit` gains `struct Fixed`, `Angle`, `FixedVec3` and `FixedQuat`.
  - Addition and subtraction use `&+` and `&-`. Swift's `+` traps on overflow, and the engine's rule
    is wrapping.
  - Multiplication uses `Int64.multipliedFullWidth(by:)`, which is exact, followed by the same
    rounding as C++.
  - Division uses `dividingFullWidth`, with C++'s zero-divisor rule applied before the call.
  - Transcendentals call the ABI entries. A second copy in Swift would be a second implementation to
    keep bit-identical, so they are not ported.
- **The same golden vectors.** A `CyberdyneKit` test reads `tools/detmath/vectors/` and checks the
  Swift arithmetic against the committed outputs. It uses `FakeEngine` for the transcendental
  entries, with values taken from the vector file. So the Swift `*` and the C++ `*` are held to one
  answer.
- **Phase rule.** In a session whose profile is `CrossPlatform` or `Lockstep`, the float-typed ABI
  setters refuse a component field declared `Fixed`, with `CY_RESULT_PERMISSION_DENIED`. The
  `docs/guides/swift.md` determinism rules gain one line: "use `Fixed` for anything a fixed step
  writes to authoritative state".

## 14. Risks and trade-offs

- **Range discipline is new to gameplay programmers.** Wrapping overflow is deterministic but wrong.
  Mitigations: the development-build `OverflowGuard`, `WideFixed` returned by every product-shaped
  query, and `saturating_*` for clamps.
- **Two scalar types in navigation.** The scalar-policy template doubles what is compiled for crowd
  steering and the funnel. We accept that rather than maintain two diverging copies.
- **The four-leg claim is only as wide as the CI matrix.** iOS, Android, Windows arm64 and macOS
  x86-64 are not covered. The README and the profile documentation say so. Adding a leg widens the
  claim and needs no other code change.
- **Kernel version changes invalidate replays.** Any coefficient change bumps `kKernelVersion`. That
  makes every recorded `CrossPlatform` replay `Incompatible` with a reason, which is the intended
  behaviour.
- **The 48-byte payload limit** allows two `FixedVec3` values. A command needing more carries an
  asset reference, which is already the rule.
- **Jolt stays presentation under `Lockstep`.** Games that want authoritative rigid bodies across
  platforms wait for the stage 9 measurement and a separate change.

## 15. Open questions

- Should the kernel format move from Q2.62 to Q1.63 for `sin` and `cos`? That gains one bit at the
  cost of an asymmetric range. Stage 2 decides from the oracle's measured worst case.
- Whether `Fixed16` earns its place should be checked against stage 6's movement benchmark. If no
  consumer stores it, it is removed before the module is declared stable rather than kept unused,
  following the same reasoning `fp_policy.h` gives for not shipping a `tgamma` replacement.
