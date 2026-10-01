# Proposal

## Why

`simulation-and-determinism` requires deterministic math types for the `CrossPlatform` and
`Lockstep` profiles: fixed-point scalars, vectors, angles and deterministic transcendental
approximations, "provided as an optional module". The module does not exist. The consequences are
already in the tree and in the requirements map:

- `DeterminismConfiguration::require()` (`src/core/determinism/src/profile.cpp`) refuses every
  `CrossPlatform` and `Lockstep` session with `ProfileRefusal::DeterministicMathMissing`, and
  `unit.determinism`'s "determinism: CrossPlatform is refused because this tree has no deterministic
  math" holds that refusal in place.
- `cy::networking::profile_required_by(NetworkMode::Lockstep)` asks only for `SamePlatform`, and
  `join_verdict()` refuses any lockstep peer whose platform, architecture or build differs. That is
  `networking-and-replication`'s "Cross-platform lockstep SHALL NOT be supported", whose stated
  reason is "the engine provides no fixed-point or soft-float simulation path".
- `cy::physics::validate_session()` rejects `CrossPlatform` and `Lockstep` with authoritative
  physics, and points at "the deterministic math path of `simulation-and-determinism`" as the only
  way out. There is no such path to take.
- The M11.e requirements sweep (PR #78) lists the module as the first of its fourteen not-built
  gaps. Its exemptions for `core-math` "Precision" and `simulation-and-determinism` "Floating-point
  policy" both name the module, and the case that would replace each exemption: one authoritative
  computation run at two vector widths with identical results, and an authoritative movement step
  in fixed point with identical hashes on every `cross_leg` architecture.

The cross-architecture comparison already exists. `cross-leg-publish` and `cross-leg-compare` in
`.github/workflows/ci.yml` publish a digest from linux-x86_64, linux-arm64, macos-arm64 and
windows-x86_64, and `tools/ci/cross_leg_digests.py` compares them. What is missing is arithmetic
whose result does not depend on which of those machines ran it.

## What Changes

This change is a design. It adds no code. It specifies the module and the changes to the
capabilities that use it, and `tasks.md` splits the build into stages that can each merge on their
own.

- **A new optional module, `cy::core-detmath`** (`src/core/detmath/`, layer 0). It contains:
  - `Fixed`, a Q32.32 scalar in an `i64`, used for all authoritative arithmetic;
  - `Fixed16`, a Q16.16 storage form in an `i32`, used for compact storage and never for arithmetic;
  - `Angle`, a binary angle in a `u32` where a full turn is 2^32;
  - `FixedVec2`, `FixedVec3`, `FixedQuat`, `FixedTransform`, `FixedAabb` and the wide (128-bit)
    accumulators used for dot products and squared lengths.

  Every operation is built from integer instructions. C++20 defines integer behaviour exactly,
  including two's-complement conversion, so the result is the same on every target.
- **Deterministic transcendentals, with stated error bounds**: `sqrt` (correctly rounded),
  `sin`/`cos`/`tan` of an `Angle`, `atan`/`atan2` returning an `Angle`, `asin`/`acos`,
  `exp2`/`log2`, and `exp`/`log` built on those two. Each uses range reduction and a fixed-point
  polynomial. The coefficients are integer constants produced by a committed generator script.
- **A conversion boundary.** Authored floats become `Fixed` once, at cook or at session
  configuration, and are never converted again per tick. `Fixed` becomes `f32` only for
  presentation, relative to the camera, and never flows back.
- **Proof across architectures.** Two kinds of evidence:
  - golden vectors and a committed kernel digest, which every CI leg must reproduce on its own;
  - a new `detmath` claim in `determinism.cross_leg` and `tools/ci/cross_leg_digests.py`, which
    compares the kernel digest and a fixed-point movement digest between architectures.

  Property tests check every function against a high-precision oracle and its declared error bound.
- **Profile acceptance.** `CrossPlatform` and `Lockstep` stop being refused as a whole. They are
  accepted when the deterministic math module is linked and every authoritative subsystem declares
  that it meets the profile. A refusal then names the subsystem that is still float-based, as the
  requirement already demands. Today that list includes Jolt-authoritative physics, abilities
  attributes, AI utility scoring and root motion.
- **Consumers under `Lockstep`**:
  - a fixed-point kinematic movement step for authoritative units, with physics classified
    `NonAuthoritative`;
  - a fixed-point path for navigation queries, flow fields and crowd steering;
  - `Fixed` payloads in gameplay commands;
  - raw-integer `Fixed` values across the C ABI, with a Swift `Fixed` type whose arithmetic matches
    the C++ arithmetic bit for bit.
- **Cross-platform lockstep becomes supported** for sessions that pass configuration. In a
  `Lockstep`-profile session, the compatibility scope stops comparing platform, architecture and
  binary build. It compares the simulation's identity instead: the source revision, the
  deterministic math kernel version, and the schema set and content manifest hashes.

Jolt's `CROSS_PLATFORM_DETERMINISTIC` mode stays switched on, but the engine still claims only its
same-platform half. Allowing authoritative rigid bodies across platforms is left to a later change,
and only after a cross-leg physics digest has been measured. Stage 9 adds that measurement but does
not make the claim.

## Capabilities

### New Capabilities

- `deterministic-math`: the formats, the arithmetic and rounding rules, the transcendentals and
  their error bounds, the conversion boundary, versioning, the performance budget and the
  cross-architecture proof.

### Modified Capabilities

- `simulation-and-determinism`: the floating-point policy names the mechanism and the condition
  under which `CrossPlatform` and `Lockstep` are accepted.
- `core-math`: the precision requirement points at the module and states that `core/math` stays
  `f32`.
- `networking-and-replication`: cross-platform lockstep is supported when the session's profile is
  `Lockstep`, and the compatibility scope depends on the profile.
- `physics`: the determinism requirement states that Jolt's cross-platform mode is not claimed,
  and states the evidence a later claim would need.
- `navigation`: a fixed-point query, flow-field and crowd path for `Lockstep` sessions.
- `gameplay-framework`: authoritative quantities in commands and in authoritative state are `Fixed`
  under cross-platform profiles.
- `replay-and-rollback`: replay compatibility records the deterministic math kernel version, and
  cross-platform reproducibility does not key on the binary.
- `native-abi`: deterministic math values cross the ABI as raw integers.
- `swift-scripting`: a Swift `Fixed` family whose arithmetic matches the engine's, checked against
  the same golden vectors.

## Impact

- New: `src/core/detmath/` with its tests, `tools/detmath/` (coefficient and golden-vector
  generators), and benchmarks under `benchmarks/`.
- Changed, in later stages:
  - `src/core/determinism/` (`BuildConfiguration`, the profile test and the determinism lint);
  - `src/networking/` (`profile_required_by`, `CompatibilityScope`, `join_verdict`);
  - `src/replay/` (`CompatibilityManifest`);
  - `src/navigation/` and `src/gameplay/`;
  - `src/abi/` and `bindings/swift/`;
  - `tests/determinism/test_cross_leg.cpp` and `tools/ci/cross_leg_digests.py`.
- `core/math` does not change. Rendering, animation, effects and every non-authoritative path keep
  `f32`.
- No existing session changes behaviour. `SamePlatform`, `ReplayStable` and `None` sessions do not
  link the module. A `CrossPlatform` or `Lockstep` session that is refused today stays refused until
  stage 5 lands. After that it is refused with the name of a subsystem instead of
  `DeterministicMathMissing`.
- The claim covers the four architecture and OS legs that `cross-leg-publish` runs. macOS x86-64 and
  Windows arm64 are not covered until CI has legs for them.
- Requirements-map follow-up: once the stages land, the `exempt:m11e` entries for `core-math`
  "Precision" and `simulation-and-determinism` "Floating-point policy" are replaced by the cases
  named in `tasks.md`. Two cases assert today's refusal and are rewritten:
  `networking-and-replication` "Lockstep requirements and limits" is mapped to
  `networking: this engine does not claim cross-architecture lockstep` in
  `src/networking/tests/test_mode.cpp`, and `unit.determinism` holds the `CrossPlatform` refusal.
  Both are rewritten in the stage that changes the behaviour they assert.
