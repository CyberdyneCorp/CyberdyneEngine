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
