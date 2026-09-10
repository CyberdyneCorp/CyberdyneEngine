## ADDED Requirements

### Requirement: The determinism firewall names where it is enforced
`vfx-system` states that VFX "SHALL NOT write gameplay state" and `ml-inference` states that model
output "SHALL NOT drive authoritative gameplay state ... unless the session is pinned". Both state
the rule from the producer's side and **neither names the point at which it is enforced**. A rule
stated on the producer's side and enforced nowhere is a convention, and a convention is not a
firewall.

The engine SHALL name a single enforcement point for both, and it SHALL be a point every
authoritative write passes through — not a review, not a naming convention, and not a diagnostic
that a caller may choose to read.

Development builds SHALL report a refused write naming the writing system, the component and the
path, per `vfx-system`'s "Development builds SHALL detect and report attempts to write replicated
or physics-owned components from VFX-driven code paths".

#### Scenario: A VFX readback attempts an authoritative write
- **WHEN** code driven by a VFX readback writes a replicated or physics-owned component
- **THEN** the write SHALL be refused at the enforcement point and reported naming the writer

#### Scenario: A non-pinned model feeds an authoritative decision
- **WHEN** a non-pinned inference result reaches a node declared authoritative
- **THEN** cooking SHALL fail, rather than the mismatch surfacing later as a desync

#### Scenario: Presentation is unaffected
- **WHEN** a VFX collision readback triggers an audio one-shot, or a model drives a blend weight
- **THEN** it SHALL be permitted, because neither is gameplay state

### Requirement: The firewall's test is a negative control
The test that proves the firewall SHALL fail when the firewall is removed, and that SHALL be
demonstrated rather than assumed.

A test whose subject is deferred SHALL NOT be recorded as satisfied: M8.b carried this criterion
with neither VFX nor inference in the tree, which is a criterion its milestone satisfies vacuously,
and it moved here with its subjects for that reason.

#### Scenario: The firewall is removed
- **WHEN** the enforcement point is disabled
- **THEN** the firewall's own test SHALL fail, and the demonstration SHALL be recorded

#### Scenario: Re-simulation is unaffected by either system
- **WHEN** a simulation is replayed from its inputs with particles and inference live
- **THEN** the state digest SHALL be identical to the run without them
