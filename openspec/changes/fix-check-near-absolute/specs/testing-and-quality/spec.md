# testing-and-quality Spec Delta

## MODIFIED Requirements

### Requirement: Test infrastructure
The test harness SHALL provide: scene and world fixtures, a deterministic clock, seeded random
generators, a mock platform and display server, an in-memory filesystem mount, network condition
simulation, image comparison utilities, and state hashing.

Tests SHALL be runnable individually and by pattern, in parallel where isolated, and SHALL produce
machine-readable results for CI.

Every floating-point comparison the harness provides SHALL state whether its tolerance is an
absolute amount or a fraction of the magnitude compared, and the harness's plain near-comparison
SHALL be absolute: a value passes when it differs from the expected value by no more than the
stated tolerance, whatever the magnitude of either.

#### Scenario: Isolated parallel tests
- **WHEN** tests run in parallel
- **THEN** each SHALL use its own world, filesystem mount, and allocator scope, so no test can
  affect another

#### Scenario: A near comparison on a large value
- **WHEN** a test checks a value of 1000.5 against 1000.0 with a tolerance of 0.01
- **THEN** the check SHALL fail, and only a comparison written as relative SHALL scale its
  tolerance by the magnitude
