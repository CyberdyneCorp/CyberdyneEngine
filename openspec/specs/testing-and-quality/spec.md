# testing-and-quality Specification

## Purpose

Defines how CyberdyneEngine is kept correct and fast: the test taxonomy and where each kind
lives, determinism and golden-image testing, performance benchmarking with regression detection,
static analysis, and the review gates a change must pass.

## Requirements

### Requirement: Test taxonomy
The engine SHALL maintain these kinds of tests, each with a defined location and runtime budget:

| Kind | Location | Budget | Purpose |
|---|---|---|---|
| Unit | `tests/unit/<module>/` | < 1 ms each | Pure logic: math, containers, algorithms, serialization |
| Integration | `tests/integration/` | < 1 s each | Subsystems together: ECS + physics, asset load + render |
| Golden image | `tests/render/` | < 5 s each | Rendering correctness against reference images |
| Determinism | `tests/determinism/` | < 10 s each | Reproducibility of simulation and replication |
| Performance | `benchmarks/` | Measured | Throughput and latency with regression thresholds |
| Swift API | `bindings/swift/Tests/` | < 1 s each | The Swift overlay and macros |
| Smoke | `tests/smoke/` | < 30 s each | The engine starts, loads a scene, renders, and exits cleanly |

Unit and integration tests SHALL run on every pull request; the full set SHALL run nightly.

#### Scenario: Fast feedback
- **WHEN** a contributor runs the pre-commit test set
- **THEN** unit tests SHALL complete in under a minute on a typical development machine

#### Scenario: Test placement
- **WHEN** a test requires a GPU or takes over a second
- **THEN** it SHALL live in the integration or render suites, not among the unit tests

### Requirement: Testability requirements on the engine
Subsystems SHALL be testable without a full engine instance:

- Servers SHALL be constructible standalone with null backends
- The ECS world SHALL be constructible without rendering, audio, or platform services
- The RHI SHALL have a null backend so render graph construction is testable headlessly
- Determinism SHALL be achievable through the fixed-step and deterministic-scheduling modes
- Time, random number generation, and input SHALL be injectable rather than globally sourced

#### Scenario: Headless render test
- **WHEN** a test constructs a render graph with the null backend
- **THEN** culling, scheduling, aliasing, and barrier insertion SHALL be verifiable without a GPU

#### Scenario: Injectable time
- **WHEN** a test advances simulation
- **THEN** it SHALL supply the time step explicitly rather than depending on wall-clock time

### Requirement: Regression tests accompany fixes
A change that fixes a defect SHALL include a test that fails without the fix and passes with it,
whenever the defect is reachable from the test harness.

Where a defect is not reachable (a driver-specific rendering bug, a platform-specific crash), the
change SHALL document why and what manual verification was performed.

#### Scenario: Bug fix without a test
- **WHEN** a fix is proposed with no accompanying test and no documented reason
- **THEN** review SHALL request one before merge

### Requirement: Golden-image rendering tests
Rendering correctness SHALL be verified by rendering fixed scenes with a fixed camera and
deterministic settings, and comparing against committed reference images using a perceptual
difference metric with a per-test tolerance.

Scenes using temporally converging illumination SHALL be captured in **converged mode** (see
`rendering-global-illumination`), so a temporally accumulated result is reproducible rather than
dependent on the number of frames rendered. A test SHALL fail if convergence is not reached within
its frame cap, rather than capturing a partially converged image.

Tests SHALL run against every enabled RHI backend, and SHALL record which backend produced a
failure.

The suite SHALL additionally support **reference comparison** against the offline path tracer for
a set of illumination scenes, reporting error rather than asserting pixel equality, so a
regression in the real-time approximation is measurable.

Reference images SHALL be regenerated only through a deliberate, reviewed step, and the diff SHALL
be inspectable in review.

#### Scenario: Unintended visual change
- **WHEN** a change alters shading in an unrelated area
- **THEN** the affected golden tests SHALL fail with a visual diff attached to the CI result

#### Scenario: Intended visual change
- **WHEN** a change deliberately improves output
- **THEN** references SHALL be regenerated in the same pull request, with the before-and-after
  images reviewed

#### Scenario: Backend divergence
- **WHEN** Vulkan and Metal produce results differing beyond tolerance
- **THEN** the test SHALL fail identifying both, since backend parity is a requirement

#### Scenario: Unconverged capture fails rather than flakes
- **WHEN** a GI scene does not converge within its frame cap
- **THEN** the test SHALL fail with that reason, rather than capturing an unstable image and
  failing intermittently

#### Scenario: Approximation error is tracked
- **WHEN** the real-time illumination result is compared against the path-traced reference
- **THEN** the error SHALL be reported and regressions in it SHALL be visible in review

### Requirement: Determinism tests
The engine SHALL verify that, for each determinism profile a project declares (see
`simulation-and-determinism`):

- running the same simulation twice produces identical state hashes per tick
- results are identical across **different worker counts** and under **chaos scheduling**, since
  both expose undeclared ordering dependencies
- re-simulation during network reconciliation reproduces the original result
- physics produces identical results for the same inputs within its declared policy
- for `CrossPlatform`, results agree across the platforms the project targets

State hashing SHALL be **hierarchical**, isolating the first diverging tick and narrowing to the
entity, component, and field that differ.

The suite SHALL maintain **golden replays**: recorded sessions with committed expected hashes,
replayed in continuous integration. An intentional behaviour change SHALL update them in the same
change with a recorded justification, so a determinism regression and a deliberate change are
distinguishable.

**Replay and save fuzzing** SHALL be included: generated command streams recorded and replayed with
hash comparison, and malformed saves — truncated chunks, corrupt hashes, unknown fields, older
schemas, missing plugins, duplicate identities — which SHALL fail diagnostically and SHALL NEVER
crash.

**Transactional save tests** SHALL simulate failure after each write phase and verify the previous
save remains valid.

#### Scenario: Non-determinism introduced
- **WHEN** a change makes system execution order depend on thread timing
- **THEN** the determinism test SHALL fail identifying the first diverging tick

#### Scenario: Hash granularity
- **WHEN** divergence is detected
- **THEN** the report SHALL narrow to the entity and component that differ, not merely report that
  hashes differ

#### Scenario: A player session becomes a test
- **WHEN** a recorded session exposes a defect
- **THEN** it SHALL be addable as a golden replay with expected hashes

#### Scenario: A malformed save never crashes
- **WHEN** a fuzzed save is loaded
- **THEN** it SHALL fail with a structured diagnostic

### Requirement: Performance benchmarks
The engine SHALL maintain benchmarks covering: ECS iteration and structural change throughput,
job system scheduling overhead, culling throughput, draw submission and render graph compilation,
physics step time at defined body counts, asset load and cook time, Swift call overhead across
the ABI, and **gameplay framework overhead** against the targets it declares.

The suite SHALL include **acceptance scenarios** exercising the architecture rather than a
favourable case:

| Scenario | Exercises |
|---|---|
| **Strategy stress** | 8 participants, 4 teams, 100 000 units with 20 000 moving, 5 000 agent groups, 1 000 structures, hundreds of commands per frame, world streaming, network authority, and replay recording |
| **Control handover** | 4-player networked co-operative play: a player enters and exits a vehicle, an AI takes it, a second player operates the turret, prediction and control transfer, a spectator observes, and a replay reconstructs it |
| **Headless server** | A dedicated server with no renderer, GPU, audio, or interface, running 100 000 entities with AI, commands, and physics at a fixed simulation rate |

The strategy scenario SHALL be treated as the primary architectural test, since a single-character
scenario does not distinguish a data-oriented gameplay framework from an object-oriented one.

Benchmarks SHALL run nightly on stable hardware, record results over time, and fail when a metric
regresses beyond a per-benchmark threshold.

Each benchmark SHALL declare what it measures and what a regression would mean, so a failure is
actionable rather than mysterious.

#### Scenario: Regression detected
- **WHEN** ECS iteration throughput drops more than the threshold
- **THEN** the nightly run SHALL fail with the commit range and a history chart

#### Scenario: Intentional trade-off
- **WHEN** a change trades throughput for a correctness fix
- **THEN** the threshold SHALL be updated in the same change with a recorded justification

#### Scenario: The architecture is tested, not a demo
- **WHEN** gameplay framework performance is assessed
- **THEN** the strategy stress scenario SHALL be the reference, and framework overhead SHALL be
  reported as a fraction of simulation time

#### Scenario: Headless is verified continuously
- **WHEN** the headless scenario runs
- **THEN** it SHALL execute with no rendering, audio, or interface code linked, and a dependency on
  any of them SHALL fail the build

### Requirement: Memory and concurrency correctness
CI SHALL run, at least nightly: AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer
builds over the unit and integration suites.

The engine SHALL report allocation leaks at shutdown in development builds, and leaks SHALL fail
the test run.

The sanitizer gate SHALL name **which suites run under which sanitizer**, and that set SHALL be
executed by continuous integration rather than merely made available as a build option. A sanitizer
that is wired but never run is not a gate.

A subsystem that **intentionally holds an allocation for the life of the process** — a pooled ring,
a registry, an interned table — SHALL declare it where the leak detector can see the declaration, so
that a report is either a real defect or a declared exception, and never a standing failure the
suite is expected to tolerate.

#### Scenario: A wired sanitizer is actually run
- **WHEN** a sanitizer is enabled by a build option
- **THEN** a named suite SHALL run under it in continuous integration, or the option SHALL be
  documented as developer-only and excluded from the gate set

#### Scenario: Intentional lifetime allocation is declared
- **WHEN** a subsystem pools memory for the life of the process
- **THEN** the leak detector SHALL be told, and the suite SHALL be green rather than expected-red

#### Scenario: Data race
- **WHEN** a system writes a component it did not declare
- **THEN** either the access assertion or ThreadSanitizer SHALL catch it

#### Scenario: Leak in a test
- **WHEN** a test leaves allocations outstanding
- **THEN** the run SHALL fail with the tag and call site

### Requirement: Static analysis and formatting
The repository SHALL enforce, via pre-commit hooks and CI:

- `clang-format` for C++, with a committed configuration
- `clang-tidy` with a curated check set, treating findings as errors
- `swift-format` for Swift
- formatting and linting for build scripts and tooling
- spelling checks on documentation and comments
- a licence header check on every source file
- a check that public API changes are documented

#### Scenario: Formatting is not a review topic
- **WHEN** code is submitted
- **THEN** formatting SHALL be enforced automatically, so review discusses design rather than
  whitespace

### Requirement: ABI and API stability gates
CI SHALL diff the generated ABI description against the committed baseline and fail on any
non-additive change unless accompanied by a reviewed approval entry recording the rationale and
version bump.

CI SHALL verify that the committed Swift overlay matches what the generator produces from the
current ABI.

#### Scenario: Accidental ABI break
- **WHEN** an existing ABI function's signature changes
- **THEN** CI SHALL fail with the diff, before the change can reach users

### Requirement: Documentation as a gate
Every public API — C++ headers, the C ABI, and the Swift overlay — SHALL carry documentation
comments, and CI SHALL fail when a newly exported symbol is undocumented.

Every specification in `openspec/specs/` SHALL be validated structurally in CI.

#### Scenario: Undocumented public API
- **WHEN** a new public function is added without documentation
- **THEN** CI SHALL fail naming the symbol

#### Scenario: Specification drift
- **WHEN** a change alters behaviour a specification describes
- **THEN** review SHALL require the specification to be updated through the OpenSpec change flow

### Requirement: Test infrastructure
The test harness SHALL provide: scene and world fixtures, a deterministic clock, seeded random
generators, a mock platform and display server, an in-memory filesystem mount, network condition
simulation, image comparison utilities, and state hashing.

Tests SHALL be runnable individually and by pattern, in parallel where isolated, and SHALL produce
machine-readable results for CI.

#### Scenario: Isolated parallel tests
- **WHEN** tests run in parallel
- **THEN** each SHALL use its own world, filesystem mount, and allocator scope, so no test can
  affect another

### Requirement: Quality gates for merge
A change SHALL NOT merge unless: all platform builds succeed, unit and integration tests pass,
static analysis and formatting pass, the ABI baseline check passes, generated code is current,
new public API is documented, and a defect fix includes a regression test or a documented reason.

The exit criteria of every milestone already reached SHALL be part of this gate set. A milestone's
criteria join continuous integration when the milestone closes and SHALL remain green afterwards; a
change that breaks an earlier milestone's criterion SHALL NOT merge unless the same change lands
the criterion's recorded replacement.

#### Scenario: Gate cannot be bypassed silently
- **WHEN** a gate fails
- **THEN** merging SHALL require an explicit, recorded override rather than a quiet exception

#### Scenario: A closed milestone stays closed
- **WHEN** a change breaks a sample or check that closed an earlier milestone
- **THEN** the merge SHALL be blocked until the criterion passes again or its replacement lands in
  the same change
