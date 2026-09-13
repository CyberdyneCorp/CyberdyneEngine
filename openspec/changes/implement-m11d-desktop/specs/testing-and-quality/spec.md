## ADDED Requirements

### Requirement: A declared gate exists, runs, and has been seen red
`Static analysis and formatting` and `Documentation as a gate` list the checks the repository
enforces. Four of them do not exist: `swift-format`, the licence-header check, the spelling check and
the undocumented-symbol gate — the last being the documentation gate the 1.0 exit criteria name.

A gate listed in a specification and absent from the repository is worse than an acknowledged
omission, because a reader of the specification believes it is running. The engine SHALL therefore
hold, for every gate this specification declares:

- an invocation that a developer can run by name and that continuous integration runs;
- a demonstration that it **fails** on an input that violates it, recorded with the change that adds
  the gate;
- and, where a gate is deliberately not yet enforced, a single recorded declaration naming the
  milestone at which it joins — so "not yet" is a decision with a rung rather than an absence.

**A gate that has never been observed red is not a gate.** This project has shipped criteria that
executed nothing and a criterion that passed with the mechanism it checked deleted; the demonstration
is what separates a check from a claim.

#### Scenario: Every declared gate is invocable
- **WHEN** the declared gate set is enumerated
- **THEN** each SHALL have a runnable invocation, and a declared gate with none SHALL be a failure
  naming the gate

#### Scenario: A gate is demonstrated to fail
- **WHEN** a gate is added
- **THEN** the change SHALL record an input that makes it fail and the observed failure, rather than
  only a green run

#### Scenario: A deferred gate is declared with its rung
- **WHEN** a declared gate is not yet enforced
- **THEN** one recorded declaration SHALL name it and the milestone at which it becomes enforced

### Requirement: The acceptance scenarios exist as tests
This specification names scenarios the engine is expected to be exercised by — a strategy-scale
stress scenario as the reference for framework overhead, a control-handover scenario, and a headless
server scenario — and none of them exists as a runnable test.

Each SHALL exist as a test in the kind whose location and budget fit it, and each SHALL assert the
property it was named for rather than merely running: framework overhead as a measured fraction
against a threshold for the stress scenario, the observable state that must survive a handover for
the second, and the observability a headless server must provide for the third.

A scenario that cannot be placed within an existing kind's budget SHALL be moved to the kind whose
budget fits and SHALL say so, which is this specification's own rule for a marginal case.

#### Scenario: A named scenario has a test
- **WHEN** this specification names an acceptance scenario
- **THEN** a registered test SHALL exercise it, and the absence of one SHALL be reported naming the
  scenario

#### Scenario: The stress scenario measures overhead rather than passing
- **WHEN** the strategy stress scenario runs
- **THEN** framework overhead SHALL be measured and compared against a committed threshold, and a
  regression SHALL fail the suite

## MODIFIED Requirements

### Requirement: Golden-image rendering tests
Rendering correctness SHALL be verified by rendering fixed scenes with a fixed camera and
deterministic settings, and comparing against committed reference images using a perceptual
difference metric with a per-test tolerance.

Scenes using temporally converging illumination SHALL be captured in **converged mode** (see
`rendering-global-illumination`), so a temporally accumulated result is reproducible rather than
dependent on the number of frames rendered. A test SHALL fail if convergence is not reached within
its frame cap, rather than capturing a partially converged image.

Tests SHALL run against every enabled RHI backend, and SHALL record which backend produced a
failure. **The reference set SHALL be shared across backends rather than committed per backend**: a
per-backend reference makes each backend its own truth, and no divergence could then be detected.

Every result SHALL record **which device produced it** — hardware, virtualised, or a software adapter
— because a comparison against a reference photographed on hardware means something different in each
case.

Where no available machine can present a device for an enabled backend, the suite's result for that
backend SHALL be reported as **not evaluated**, with the reason, and SHALL NOT be reported as a pass.
A rendering claim resting on a machine that cannot render is the failure mode this rule exists to
prevent.

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

#### Scenario: A backend with no device is not a pass
- **WHEN** an enabled backend has no machine able to present a device
- **THEN** its golden results SHALL be reported as not evaluated with the reason, and the milestone
  record SHALL carry that rather than a green result

#### Scenario: The device is named in the result
- **WHEN** a golden comparison completes
- **THEN** the result SHALL name the backend and the device class that produced it
