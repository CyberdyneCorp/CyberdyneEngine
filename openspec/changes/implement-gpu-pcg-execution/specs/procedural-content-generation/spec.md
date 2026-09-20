## ADDED Requirements

### Requirement: GPU PCG conformance workload

The engine SHALL provide a GPU-executable PCG workload whose records and canonical digest can be
compared with a CPU reference for the same seed and parameters. The workload SHALL use arithmetic
with defined cross-platform integer semantics and SHALL preserve candidate-slot order.

#### Scenario: CPU and GPU agree

- **GIVEN** the same seed, candidate count, and density threshold
- **WHEN** the workload executes through a supported GPU backend
- **THEN** every GPU candidate record SHALL equal the CPU reference record at the same slot
- **AND** their canonical digests SHALL be equal

#### Scenario: The conformance workload is meaningful

- **WHEN** the conformance workload runs
- **THEN** it SHALL produce at least one accepted and one rejected candidate
- **AND** changing the seed SHALL change the canonical digest

#### Scenario: Missing execution is visible

- **WHEN** no supported GPU device or shader path is available
- **THEN** the conformance run SHALL fail naming the unavailable GPU path
- **AND** it SHALL NOT report CPU/GPU agreement
