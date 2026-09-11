# `src/core/determinism/` — the simulation's sense of time, chance, state and order

Layer 0, `cy::core-determinism`, headers `<cy/core/determinism/*.h>`, namespace `cy::determinism`.

Governed by `openspec/specs/simulation-and-determinism/spec.md`, which reaches **Seed** at M2.
design.md §5 says what Seed means, and it is worth restating before anything below is trusted:

> Seed here means *the shape is right and the hooks exist*, not that anything is validated.

**At M2 nothing in this module validated anything**, and that was the point: what landed first was
the vocabulary everything from M3 to M8 would be written against, because a classification
retrofitted onto components that already exist is a classification nobody applies.

**M9 added the half that validates.** `profile.h` is the contract and the refusal, `fp_policy.h` is
the floating-point policy the milestone's spike measured, `codec.h` is the generated codecs,
`validator.h` is the divergence localiser and the chaos harness, and `lint/determinism_lint.py` is
the static half. The four claims below are M2's and still hold; §5 onwards is M9's and says what the
new files promise and — as carefully — what they do not.

| File | Task | What it owns |
|---|---|---|
| `epoch.h` | 4.2.2 | `Epoch`, `SimulationPoint`, `is_stale`, `EpochCounter` |
| `clock.h` | 4.2.1 | `TickRate` as an exact rational, `SimulationClock`, `TickMode` |
| `commit.h` | 4.2.3 | `TickPhase`, `CommitRecord`, `CommitObserver`, `CommitBoundary` |
| `random.h` | 4.2.4 | `StreamId`, `RandomStream`, `RandomSource`, `SampleCursor` |
| `classification.h` | 4.2.5 | `SimulationClass`, `Classified<>`, the firewall, `ExternalResult` |
| `state_schema.h` | 4.2.5/6 | `StateSchema` — what participates in the hash, **declared** |
| `hash.h` | 4.2.6, M9 2.5 | `StateHashTree`, `Divergence`, `HashSchedule`. M9 added `Divergence::missing_*`: a shape mismatch now names the child that has no peer, which the path cannot, because the deepest node the two trees share is that child's *parent*. |
| `provider.h` | 4.2.6 | `StateProvider`, `StateProviderRegistry` |
| `ordering.h` | 4.2.7 | `Ordering`, `select_best`, `sort_by_key` |
| `profile.h` | M9 2.1/2.2 | `DeterminismProfile`, `BuildConfiguration`, the configuration-time refusal, `NonFiniteGuard` |
| `fp_policy.h` | M9 2.3 | The measured `<cmath>` table, `is_permitted_under`, `ulp_distance`, and twelve replacements |
| `codec.h` | M9 2.4 | `StateCodec` — one compiled plan per subject per purpose |
| `validator.h` | M9 2.5 | `TickHashComparison`, `localise`, `chaos_conditions`, `validate_scenario` |
| `cmake/determinism_profile.cmake` | M9 2.2 | `cy_declare_determinism_profile()` — the configure-time refusal |
| `lint/determinism_lint.py` | M9 2.6 | The determinism lint: five source rules and the build half |

The walk that binds the hash to an `ecs::World` is **not here** — layer 0 cannot name an entity. It
is `src/runtime/state_hash.h`, at layer 5.

## The four claims worth arguing with

### 1. The illegal read is unspellable — for classified state, and only for it

design.md §5 sets the bar and names the trap: M1's "workers never block" is enforced for *declared*
blocking, and an undeclared `read()` is caught only by a watchdog, so M9 would inherit a validator
that reports clean on code that is not.

**What is genuinely unspellable.** A firewall crossing between two values held in `Classified<>`
does not compile. `read()` takes an `AccessContext<C>` witness and is constrained on
`may_read(C, source)`, so an authoritative system cannot name the value inside a `Presentation<f32>`
— there is no expression that yields it. The same closes the feedback direction: a presentation
context cannot write an authoritative field. And a value cannot be laundered by copying, because the
wrappers are distinct types with no converting constructor.

`tests/test_classification.cpp` proves it the only way a negative claim can be proved without
breaking the build: a `CanRead`/`CanWrite` concept, and `static_assert` that the expression is
ill-formed. A regression that made the read legal again fails the build.

**What is only refused, or not caught at all.**

* A system that reads a plain global, a raw `float`, or a member of a struct that never adopted
  `Classified<>`. Nothing here sees it. That is the determinism lint's, at M9.
* `bypass_classification()`, which every reflection-driven consumer needs. It is spelled to be ugly
  and greppable — `grep -rn bypass_classification src/` is the audit — and it is not enforced.
* `record_external()`, which is *supposed* to be spellable: the requirement asks for the crossing to
  be captured, not prevented.

So the honest statement is: **the crossing is unspellable for state that is classified, and
classification is opt-in per field.** Adopting the wrapper buys the guarantee; a component that has
not adopted it gets a comment.

**M2 closed with nothing having adopted it, and M3's task 1.3 changed that.**
`scene::InterpolatedTransform` is the first classified component: both its fields are
`Presentation`, because the component exists so a renderer can blend between two ticks, and an
authoritative system now cannot name either value — there is no overload that yields it.
`src/scene/src/propagation.cpp`'s interpolation block and `Node::render_transform()` carry a
`PresentationContext` and say why. The eleven that follow are named in `scene/components.h` with the
cost of each; the rule going forward is the one the task states, that a component is classified at
the moment it is authored, because retrofitting a witness into every reader is the expensive half.

**And the state hash learned to read a name as text rather than as a handle.** `StateField` carries
a `StateEncoding`, whose one non-`Direct` value says "these four bytes are a `cy::Name`, fold in its
text". A `Name`'s index is its position in a process-wide table filled in interning order, which
`core-type-system` says is not stable across runs — so hashing the index would make the state hash a
function of what else the process interned first. That is what lets `scene::NodeName` be hashed at
all, and it is what closes "a divergence in a node's name does not change the hash".

### 2. `SimulationClock` cannot read a wall clock

It has no member that calls one and includes no header that offers one. Its only source of elapsed
time is `accumulate()`, which the host calls with a duration it measured. A system handed a
`const SimulationClock&` — which is all `runtime::Simulation` hands out — has no wall clock reachable
through it. `Runtime::tick()` is the one place in the engine where wall time enters the simulation's
sense of time, and it says so at the line.

The tick's *duration*, which the diagnostics requirement asks for, comes from a function pointer the
host supplies in `SimulationConfig::diagnostic_clock`. It is read in two places in
`src/runtime/src/simulation.cpp` and appears in no interface a system is handed; with no such
function every duration is reported as zero.

### 3. A random draw is a pure function, and that is what makes three properties free

There is no `next()`, no cursor inside a stream, no mutable state anywhere in `random.h`. A draw is
`(seed, stream, point, entity, index) -> u64`. Parallel sampling is safe because there is nothing to
share; sampling is order-independent because the answer does not depend on what came before; and
"a new call does not shift the world" is arithmetic rather than discipline — a system that begins
drawing one more value per tick changes nothing any other system computes, *because no other
system's inputs mention it*.

The cost is that the caller supplies the sample index. That is the point: an index from a hidden
counter is exactly the shared mutable state the requirement forbids.

The mixer deliberately does **not** call `cy::hash_bytes`, which is seeded per process in
development builds. A stream keyed by that would produce a different sequence on every run.
`kMixerVersion` is what a replay header records so a mismatch is a diagnostic rather than a
divergence.

### 4. The chunk level is in the hash hierarchy's enum and not in the walk

`simulation-and-determinism` names seven levels: world, subsystem, archetype, chunk, entity,
component, field. The walk implements six of them. Chunk assignment is allocator and insertion
history, and the same specification requires that determinism not depend on allocator history and
has its validator *deliberately perturb* chunk assignment between runs — so a hash with a chunk level
in it would differ between two runs that agree about every value a game can observe. The level stays
in the enum because an incremental scheme (a per-chunk subtree hash re-folded in a stable order) is
the shape M9 will want.

## Things thinner than they look

* **Nothing is incremental.** A hash is a full walk: O(entities log entities) for the per-archetype
  sort plus one `World::get` per (entity, component). The requirement says subtree hashes SHOULD be
  incremental "where practical"; the runtime measures and reports the cost so that the decision is
  taken against a number rather than an intuition.
* **A component with no declared schema is not hashed, and is counted.** Hashing its bytes is not
  available — raw structure memory as canonical authoritative state is on the forbidden list — so
  the honest alternative is `WorldHashReport::subjects_undeclared`. At M2 that number is non-zero in
  every real world: the ECS's `Parent`/`Children` and the scene's twelve built-ins are registered by
  name with no `TypeInfo`. Covering them needs `StateSchema::declare()` with an explicit field list.
* **`StateSchema` exists because reflection has no simulation class.** M1's attribute set has
  `PersistenceKind` and no enumerator for `Predicted` or `Presentation` — the two the firewall is
  actually about. `declare_reflected()` derives what it can; the rest is declared. Adding a
  `SimulationClass` attribute to the generator would collapse the two, and that is a change to
  `src/core/reflect/`, which this milestone did not own.
* **`CommitBoundary` notifies observers in registration order, not name order** — the one place in
  this module where that is right. An observer's *effects* are outside the simulation (a file
  written, a packet sent) and are not part of any hashed state, so ordering by name would buy
  nothing and would make "the save runs before the network send" inexpressible.
* **The provider registry's `capture`/`restore` are byte-oriented and unimplemented by the two
  built-in providers.** They declare `Checkpoint` and `Save` participation and implement `hash`
  only; the base class refuses the others rather than returning empty bytes. Checkpointing is M6's
  and rollback is M9's, and a provider that silently captured nothing would be worse than one that
  says it cannot.
* **`Predicted` is never hashed.** Two peers legitimately disagree about a prediction, and hashing
  it would make correct prediction look like divergence. It is rolled back and checkpointed, because
  reconciliation needs a value to rewind.
* **`ordering.h` cannot stop a system writing its own loop over a `HashMap`.** `Ordering` is a
  declaration a query carries so the scheduler and the validator can read it; enforcing it is the
  chaos scheduler's and the lint's. What `select_best` does enforce is that there is no overload
  without a tie-break.

---

## M9: the half that validates

### 5. A determinism profile is a build-configuration contract, not only a source contract

This is M9's spike finding and it is the reason `profile.h` has a `BuildConfiguration` in it.
`openspec/changes/implement-m9-integrity/design.md` §1.2, measured over forty builds:

* The engine's own primitives agree bit-for-bit between clang 18 and GCC 13 at `-O0`, `-O1`, `-O2`
  and `-O3 -flto` — in eighteen of the spike's twenty configurations.
* The two that disagree are `-march=native` with contraction left at the compiler's default. 13 of
  16 workloads move, the two compilers disagree with *each other* in 8 of them, and **231 of 267
  values move in the `state_hash` workload** — the number a lockstep session compares.
* They agree today only because the engine targets baseline x86-64, which has no fused multiply-add
  for the default setting to use. `src/core/math/tests/CMakeLists.txt` already anticipates
  `-march=x86-64-v3` in as many words.

So two builds of identical source, differing only in `-march` and a contraction flag, produce
different state hashes. The refusal is therefore in two places, at two moments:

| Moment | Mechanism | What it refuses |
|---|---|---|
| CMake configure | `cy_declare_determinism_profile()` | a covered module compiled without `-ffp-contract=off`, or with fast-math |
| Session configuration | `DeterminismConfiguration::require()` | a subsystem that guarantees less than the session needs, a build whose flags make the profile unachievable, and `CrossPlatform`/`Lockstep` outright |

**The function does not add the flag for you.** It requires the module to have added it and fails if
it has not, because a function that quietly fixed the flag would be a check that can never fail.
Deleting `target_compile_options(cy_gameplay PRIVATE -ffp-contract=off)` and reconfiguring:

```
$ cmake -S . -B build/command-log
CMake Error at src/core/determinism/cmake/determinism_profile.cmake (message):
  Determinism profile refused at configuration.

    Module    : cy_gameplay
    Declares  : SamePlatform — the command stream and its validation are authoritative;
                CrossPlatform would need deterministic math types, which this tree does not have
    Guarantee : floating-point contraction off
    Missing   : -ffp-contract=off on this target's COMPILE_OPTIONS
    M9's spike measured two builds of identical source, differing only in -march and this
    flag, producing different state hashes: 231 of 267 values moved in the state_hash
    workload. See openspec/changes/implement-m9-integrity/design.md section 1.2.
Call Stack (most recent call first):
  src/core/determinism/cmake/determinism_profile.cmake:109 (cy_determinism_verify_profiles)
  CMakeLists.txt:DEFERRED

-- Configuring incomplete, errors occurred!    (exit 1)
```

### 6. `CrossPlatform` is refused, and that is the honest answer rather than a gap

`simulation-and-determinism` requires the `CrossPlatform` and `Lockstep` profiles to use
**deterministic math types** — fixed-point scalars and polynomial approximations — "provided as an
optional module". **This tree has no such module.** `require()` therefore refuses both profiles
outright with `ProfileRefusal::DeterministicMathMissing`, and `test_profile.cpp` asserts it, so the
day somebody adds a flag that makes the refusal go away, a test goes red and asks why.

`docs/roadmap/status.yaml` must not record `simulation-and-determinism` as claiming
cross-architecture determinism until a run has compared two architectures. **Nothing in this tree
has done so**: this host has one architecture and one operating system, and the second exists only
in the CI matrix.

### 7. Thirteen `<cmath>` functions are forbidden, and twelve have a replacement

The spike measured thirty-eight functions three ways. Three groups came out of it and `fp_policy.h`
is that table:

* **Exact by IEEE-754**: `sqrt fabs floor ceil trunc round nearbyint fma fmod remainder copysign`.
  Permitted under every profile.
* **Correctly rounded in glibc 2.39 on x86-64**: `exp exp2 log log2 sin cos tan atan tanh erf pow`.
  Zero error against a wider-precision reference on every sample point — which is a statement about
  *this libm*, so they are permitted under `SamePlatform` and refused under `CrossPlatform`.
* **Not correctly rounded here, and the compiler changes their value when it folds them**:
  `acos acosh asin asinh atan2 atanh cbrt cosh expm1 log10 log1p sinh tgamma`. Forbidden above
  `ReplayStable`.

`cy::determinism::fp` ships twelve of the thirteen, each built **only** from the first two groups.
That buys one property: whether the compiler evaluates the expression itself or leaves it to run
time, it is evaluating operations that have a unique right answer on this platform — so `fp::acos(k)`
is the same value at `-O0` and `-O3`, which `std::acos(k)` measurably is not. `tgamma` has no
replacement on purpose: it is forbidden because it was measured wrong, not because a simulation
needs it, and shipping a Lanczos approximation nobody calls would be code with no caller to keep it
honest.

**It is not a cross-platform claim.** A different libm may round `exp` differently and every
function here would move with it.

### 8. The lint has two jobs, and the second one is the spike's finding

`lint/determinism_lint.py`. The source half is the five rules the requirement names — wall-clock
reads, ambient generators, unordered iteration used as a decision order, presentation reads, and
floating-point operations the profile disallows. The build half asserts that every module declaring
a profile was **compiled** with contraction off, reading `determinism-profiles.txt` (written by the
CMake function) and `compile_commands.json` (written by CMake). Neither is a list the script
maintains, so neither can drift away from what was built.

**Coverage is a number, not a sentence.** `--report-coverage` prints how many of the built targets
declare a profile. The denominator moves as the tree grows, which is the point — it is a reading
taken on every run rather than a figure anyone maintains. At the time of writing:

```
determinism-lint: 5 of 401 built targets declare a determinism profile; 44 translation units examined.
determinism-lint: covered targets: cy_core_determinism, cy_gameplay, cy_replay,
                  cy_test_integration_determinism_scale, cy_test_unit_determinism
determinism-lint: NOT a finding — an undeclared module has claimed nothing, so there is nothing for
                  it to fail. The ratio is the gap, and it is a number rather than a sentence.
determinism-lint: no findings over 44 translation units in 5 covered targets.
```

An undeclared module is deliberately **not** a finding: it has claimed nothing, so it cannot fail a
claim. What it is, is uncovered — and the ratio says so on every run. Extending the coverage means
adding `-ffp-contract=off` and a declaration to modules this phase does not own (`src/physics/`,
`src/animation/`, `src/navigation/`, `src/ecs/`), which is a change those directories' owners make.

**Both halves are proved able to fail.** `--selftest` writes one fixture per rule and fails if the
rule did not fire, plus a clean fixture that must stay silent and an exemption that must apply to its
own rule and to no other; it is registered as `integration.determinism_lint_selftest`. The build
half was run against a flag nothing carries:

```
$ python3 src/core/determinism/lint/determinism_lint.py --root . --build build/command-log \
      --contraction-flag='-ffp-contract=on'
src/replay/src/record.cpp:0: [contraction] target 'cy_replay' declares determinism profile
    SamePlatform but was compiled without -ffp-contract=on
... 44 finding(s), severity error.   (exit 1)
```

Exemptions are **per rule and per file**, three of them, each naming the file whose *subject* is the
rule it breaks. A whole-file exemption is how a lint grows an unexamined corner; the selftest
asserts that an exemption for one rule does not silence another.

### 9. What the M9 files still do not do

* **The chaos harness does not schedule anything.** `ExecutionConditions` is a *description* a
  scenario is obliged to honour, and `validate_scenario()` refuses a set that varies none of the
  four dimensions — because a pass over identical conditions proves the scenario is a function, not
  that it is order-independent. Wiring it to `cy::jobs`' deterministic mode is the runtime's.
* **`localise()` names a field, not a system.** Attributing a write to a *system* needs a per-write
  attribution the ECS does not record, and inventing one here would be a second mechanism beside the
  firewall.
* **`StateCodec` is a compiled plan, not emitted code.** The requirement is that the hot path
  consults no metadata, and `test_codec.cpp` checks exactly that by destroying the schema before
  using the codec.
