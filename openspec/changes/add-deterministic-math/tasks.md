Each stage below is its own pull request and leaves `main` green on its own. Until stage 5, no
session's behaviour changes: `CrossPlatform` and `Lockstep` stay refused with
`DeterministicMathMissing`. Stages 6 to 9 depend on stage 5. Stage 9 can run in parallel with
stages 6 to 8.

## 0. Design (this change)

- [x] 0.1 Write `proposal.md`, `design.md`, the `deterministic-math` capability and the deltas to `simulation-and-determinism`, `core-math`, `networking-and-replication`, `physics`, `navigation`, `gameplay-framework`, `replay-and-rollback`, `native-abi` and `swift-scripting`; validate with `openspec validate add-deterministic-math --strict`.

## 1. The scalar core

- [x] 1.1 Declare `cy::core-detmath` in `src/core/detmath/CMakeLists.txt` with `cy_add_module`, layer `core`, an explicit source list, and the `-ffp-contract=off` option that `src/core/math/CMakeLists.txt` uses. Add a README stating what the module claims and what it does not.
- [x] 1.2 Add `Fixed`, `Fixed16`, `Angle` and `WideFixed` with the arithmetic rules of design §4.3. Signed wrapping goes through `u64`. Division by zero follows the defined rule. `Fixed16` defines comparison and no arithmetic operator; `OverflowGuard` is thread-scoped (an operator has no parameter to hand a guard), counts wraps, saturations and domain errors, and names the first site.
- [x] 1.3 Add the 128-bit multiply and divide: `__int128` on GCC and Clang, `_mul128` and `_div128` on MSVC x64, `__mulh` on MSVC arm64, and the portable 32-bit-limb reference compiled everywhere. MSVC x64 uses `_umul128` with the signed product built from it on every path (only the unsigned primitive varies), and `_udiv128` after dividing the high limb so it cannot fault. MSVC arm64 uses `__umulh` and the long division (Hacker's Delight `divlu`), which is compiled and tested on every leg.
- [x] 1.4 Add a correctly rounded `sqrt` over `WideFixed`, and `OverflowGuard` modelled on `cy::determinism::NonFiniteGuard`, compiled to nothing in Shipping.
- [x] 1.5 Add `tools/detmath/gen_vectors.py`, which writes the edge-case and seeded vector files under `tools/detmath/vectors/`. Add `unit.detmath` (`cy_add_test`), which checks the committed outputs, checks intrinsic against reference over a random sweep, and checks the arithmetic rules case by case. `CY_DETMATH_RECORD_GOLDEN=1` rewrites the vectors and fails the run. The rules case by case and a unit-sized intrinsic-against-reference sweep are in `unit.detmath`. Checking every committed vector, the digests and a 20 000-input reference sweep exceeds the 1 ms unit budget, so it is `integration.detmath_vectors`. The expected outputs come from `tools/detmath/model.py`, a second implementation of the rules; the sweep is 16 384 seeded inputs per function rather than ~10^5, folded into committed per-function digests (each file holds the edge cases and the first 256 seeded inputs).
- [x] 1.6 Compile the kernel translation units a second time with `-mgeneral-regs-only` where `check_cxx_compiler_flag` accepts it, and report when the flag is not accepted rather than skipping silently. The probe answer is printed at configure; `integration.detmath_variants` also requires the build on GCC and Clang for x86-64 and arm64, so it cannot quietly stop running there.
- [x] 1.7 Add `benchmarks/` cases for `*`, `/` and `sqrt` with baselines (design §11). Thresholds recorded in `benchmarks/baseline.json` (`detmath/f64-mul`, `detmath/mul`, `detmath/div`, `detmath/sqrt`); first measurement in the module README and the guide: `*` at 2.00× the `f64` multiply, `/` 9.2 ns, `sqrt` 24.6 ns.
- [x] 1.8 Falsify with `just roadmap-falsify --record`: flip the tie rule in `*` (golden vectors go red), and replace the reference multiply with a `double` product (the general-registers build goes red). Applied by hand and recorded in `src/core/detmath/README.md` rather than through `just roadmap-falsify --record`, which proves milestone-ledger criteria and has none naming these suites. The tie flip turns `unit.detmath` and the `mul` golden vectors red; the `double` reference multiply fails the `-mgeneral-regs-only` build and, without it, `unit.detmath`.

## 2. Transcendentals

- [x] 2.1 Add `tools/detmath/gen_coefficients.py`. It fits each polynomial on its reduced interval, chooses the degree that meets the declared bound, and writes a generated header containing the integer coefficients and each measured error. `just generate-check` covers it. Chebyshev fits (`mpmath.chebyfit`) with the first degree under 2^-44 (2^-46 for `exp2`); `just generate-detmath`, and `just generate-check` runs it with `--check` (mpmath pinned to 1.3.0 in CI).
- [x] 2.2 Implement `sin`, `cos`, `sincos` and `tan` of `Angle`; `atan` and `atan2` to `Angle`; `asin` and `acos`; `exp2` and `log2`; and `exp`, `log` and `pow` (for x > 0), following design §5.2. `pow` is declared with a bound of max(2 ulp, relative 2^-34 + |y| 2^-58), which the design did not state.
- [x] 2.3 Add `tools/detmath/gen_oracle.py`, which writes high-precision references as 128-bit-scaled integers. Add property cases to `unit.detmath` for each declared bound (design §5.3), monotonicity, and the binary-angle identities. Report each worst error beside its bound. The oracle files hold 2 000+ cases per function; the worst error is printed beside each bound by `integration.detmath_vectors`.
- [x] 2.4 Extend the golden vectors to every transcendental, and add `detmath::kKernelVersion`. The vectors also hold the Q2.62 kernel values (`sin_core`, `cos_core`, `atan_core`, `exp2_core`, `log2_core`): a last-bit coefficient change moves those but almost never a rounded result.
- [x] 2.5 Add benchmarks for `sin`/`cos`, `atan2`, `exp2` and `log2`. `detmath/sin` 19.7 ns, `detmath/atan2` 30.0 ns, `detmath/exp2` 26.8 ns, `detmath/log2` 26.0 ns, all inside their budgets.
- [x] 2.6 Falsify: flip the last bit of one `sin` coefficient and one `atan` coefficient. The golden vectors and, where the bound is tight, the property case go red. Applied by hand, recorded in `src/core/detmath/README.md`. Flipping the last bit of `kSin[0]` or `kAtan[0]` does NOT move the rounded `sin` or `atan2` vectors (a 2^-62 change rarely crosses a 2^-32 rounding boundary); it turns the Q2.62 core vectors, the digests and `determinism.cross_leg` red, which is why task 2.4 added those vectors.

## 3. Geometry types and integration seams

- [ ] 3.1 Add `FixedVec2`, `FixedVec3`, `Fixed16Vec3`, `Rot2`, `FixedQuat`, `FixedTransform`, `FixedAabb`, `FixedCircle` and `FixedCapsule2D` (design §6), with `WideFixed` dot products and lengths.
- [ ] 3.2 Add a `tests/test_conventions.cpp` in the module that asserts the same numeric consequences as `src/core/math/tests/test_conventions.cpp`.
- [ ] 3.3 Add the conversion boundary in its own `convert.cpp`: `from_f32_cooked`, `from_f64_cooked` and `to_f32_relative`, each with cases for exactness, rounding and range.
- [ ] 3.4 Add `RandomStream::unit_fixed()` in `src/core/determinism/include/cy/core/determinism/random.h`, with a case that shows it is exact.
- [ ] 3.5 Add the `float-on-cross-platform-path` rule to `src/core/determinism/lint/determinism_lint.py`, with a `SELFTEST_CASES` entry and a clean-fixture line.

## 4. Proof across architectures

- [x] 4.1 Add a kernel digest (every golden output folded in order) and a committed expected value. Every leg checks it on its own. `detmath::kernel_digest()` folds every function's seeded sweep (the golden inputs' generator) rather than the files' lines, so the general-registers and AVX2 builds compute it in one process; `integration.detmath_vectors` and `determinism.cross_leg` check it against `tools/detmath/vectors/digests.txt` on each leg.
- [ ] 4.2 Add a fixed-point movement scenario: 2 000 units, crowd-style separation, seeded random orders, 600 ticks, hashed per tick. It is written over the stage-3 types only, so it does not wait for stage 6.
- [ ] 4.3 Publish `detmath-kernel-digest` and `detmath-movement-digest` from `tests/determinism/test_cross_leg.cpp`. Add a `detmath` entry to `CLAIMS` in `tools/ci/cross_leg_digests.py`, with negative fixtures in `tools/ci/test_cross_leg_digests.py` (a zero digest, one architecture, an empty workload). The kernel half has landed: `detmath-kernel-version`, `detmath-sweep-count` and `detmath-kernel-digest` are published, the `detmath` claim is in `CLAIMS` with six negative fixtures, and `cross-leg-compare` runs `--detmath`. `detmath-movement-digest` waits for 4.2.
- [x] 4.4 Add the vector-width comparison: build the kernel a second time with `-mavx2` on the pattern of `src/core/math/CMakeLists.txt`'s AVX2 suite, and compare digests in one process. In `integration.detmath_variants`, per function, beside the `-mgeneral-regs-only` build.
- [ ] 4.5 Record the first four-leg comparison in `tests/determinism/README.md` with its run number, and name the legs the claim covers.
- [ ] 4.6 Falsify: fold the movement hash with `+` (`determinism.cross_leg` goes red), and delete the `detmath` claim from the comparator invocation (`tools/ci/cross_leg_audit.py` goes red).

## 5. Profile acceptance

- [x] 5.1 Make `cy::core-detmath` export `CY_DETERMINISM_MATH=1` as PUBLIC, so `BuildConfiguration::from_build()` reports the module wherever it is linked.
- [x] 5.2 Rewrite `determinism: CrossPlatform is refused because this tree has no deterministic math` into three cases: refused without the module; accepted with the module when every authoritative subsystem declares `CrossPlatform`; refused naming `physics` when physics is authoritative. The `unit.detmath` profile cases hold the other side of the seam with the real `from_build()`.
- [ ] 5.3 Networking: give `profile_required_by()` the session's declared profile; add `CompatibilityScope::simulation_identity` and `JoinRefusal::SimulationIdentityMismatch`; make `join_verdict()` compare by profile (design §12.2). Rewrite `networking: this engine does not claim cross-architecture lockstep` into a case where a cross-architecture peer with the same identity is admitted and one with a different kernel version is refused, and keep the same-platform cases unchanged.
- [ ] 5.4 Replay: add `detmath_kernel_version` and `simulation_identity` to `replay::CompatibilityManifest`, and classify cross-platform replays by identity (design §12.3). Add a case where a different kernel version is `Incompatible` with that reason.
- [ ] 5.5 Update `docs/guides/physics.md`'s session table, the `networking-and-replication` purpose paragraph (when the change is archived), and `src/core/determinism/README.md`.

## 6. Authoritative movement and navigation under `Lockstep`

- [ ] 6.1 Add the fixed-point kinematic mover (design §8): velocity integration, deterministic pairwise separation over a `Fixed` grid, a clamp to the navigation surface, and heights from cooked `Fixed16` samples. Add an `AuthoritativeTransform` component and the presentation-sync system that writes the scene transform from it.
- [ ] 6.2 Navigation: add the per-world `NavArithmetic` declaration and load-time conversion of the baked mesh. Instantiate the A* costs, funnel, flow-field integration and `Crowd` steering over the `Fixed` scalar policy. Refuse runtime rebuilds as authoritative input in `Fixed` worlds. Declare the subsystem's profile from its worlds.
- [ ] 6.3 Extend `integration.navigation_crowd` and `integration.navigation_fields` with `Fixed`-world cases whose digests join `detmath-movement-digest` on the cross-leg legs.
- [ ] 6.4 Add the 100 000-unit movement-step benchmark with its `f32` ratio (design §11).

## 7. Gameplay commands

- [ ] 7.1 Under a cross-platform profile, refuse command registration when a payload declares a floating-point field, naming the type and the field. Add a case, and a falsification that removes the check.
- [ ] 7.2 Add a lockstep case in which two peers (two sessions in one process, driven by the same command log) execute a move order carried by a `Fixed` payload, and their state hashes agree on every tick. Publish its final hash on the cross-leg legs, so that agreement between architectures is checked by the comparator rather than assumed from one host.
- [ ] 7.3 Make abilities, AI utility and root motion declare `SamePlatform` explicitly, and add a case where a `Lockstep` session using abilities authoritatively is refused naming `abilities`.

## 8. Swift and the ABI

- [ ] 8.1 Append to the next ABI minor: `CyFixed`, `CyAngle`, `CyFixedVec3`, `CyFixedQuat`, `CY_VAR_FIXED`, typed fixed component accessors, and `detmath_*` transcendental entries with span variants. Regenerate the Swift overlay and the Rust SDK. The ABI gate accepts it as an append.
- [ ] 8.2 Refuse float setters on fixed fields in `CrossPlatform` and `Lockstep` sessions with `CY_RESULT_PERMISSION_DENIED`, with a `unit.abi` case.
- [ ] 8.3 Add `Fixed`, `Angle`, `FixedVec3` and `FixedQuat` to `CyberdyneKit`, using `&+`, `&-`, `multipliedFullWidth(by:)` and `dividingFullWidth`. A package test reads `tools/detmath/vectors/` and matches every committed output.
- [ ] 8.4 Add the `Fixed` rule to the determinism rules in `docs/guides/swift.md`, and replace "Lockstep is not available yet" once stage 7 has landed the command path.

## 9. Measurements and close

- [ ] 9.1 Publish a Jolt scene's per-tick physics hash on the cross-leg legs **as a measurement**, under a field the comparator reports but no claim depends on. Record the outcome in `docs/guides/physics.md` without changing `DeterminismPolicy`.
- [ ] 9.2 Add the strategy-scale `Lockstep` scenario that `simulation-and-determinism`'s "Simulation performance and testing" names (8 participants, 100 000 units, 5 000 groups, 1, 8 and 16 workers) over the fixed-point mover. Report hash cost as a fraction of tick time.
- [ ] 9.3 Replace the `exempt:m11e` entries for `core-math` "Precision" and `simulation-and-determinism` "Floating-point policy" in `tools/roadmap/requirements-coverage.toml` with the stage 4 and stage 7 cases. The `core-math` "Precision" exemption is replaced by `integration.detmath_variants`. "Floating-point policy" stays exempt, with its note updated, until the movement digest of 4.2 agrees across the legs.
- [ ] 9.4 Write the deterministic math section of the documentation: formats, rules, bounds, the conversion boundary, the covered legs and the performance figures. Then archive this change.
