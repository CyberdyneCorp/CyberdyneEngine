## MODIFIED Requirements

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

**A budget is a design guard, not a stopwatch.** The per-test budgets above exist to keep a unit
test from becoming an integration test by accident — not to assert a wall-clock time on a machine
whose load the test does not control.

Budget enforcement SHALL therefore allow a stated tolerance for machine variance, and a case that
exceeds its budget only under load SHALL be reported as a case to reclassify rather than failing the
build outright. A test that measures the machine rather than the code produces red builds that have
nothing to do with the change under test, and the cost compounds: a criterion invoked by several
milestone ledgers, each nesting the last, multiplies one marginal case into many exposures per pull
request.

Where a case genuinely needs more time than its kind allows, the answer SHALL be to move it to the
kind whose budget fits, and to say so.

#### Scenario: A marginal case is reclassified, not tolerated
- **WHEN** a unit case exceeds its budget only under concurrent load
- **THEN** it SHALL be reported as mis-classified, and moving it to the integration suite SHALL be
  the fix rather than raising the threshold

#### Scenario: Machine noise does not fail a build
- **WHEN** the same suite passes on an idle machine and is marginal on a loaded one
- **THEN** the gate SHALL not report a defect in the change under test

#### Scenario: Fast feedback
- **WHEN** a contributor runs the pre-commit test set
- **THEN** unit tests SHALL complete in under a minute on a typical development machine

#### Scenario: Test placement
- **WHEN** a test requires a GPU or takes over a second
- **THEN** it SHALL live in the integration or render suites, not among the unit tests
