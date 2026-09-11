## ADDED Requirements

### Requirement: One producer per field is a refusal, and the refusal is a negative control
`environment-fields` requires that a field have exactly one producer, and `docs/ROADMAP.md` makes it
an exit criterion in the imperative: "One producer per field is enforced — a second producer
registration fails".

A rule enforced by convention is a rule that holds until the second team writes the second producer.
The engine SHALL therefore **refuse the registration**, naming the field, the producer already
registered and the producer being refused, and the refusal SHALL happen at registration rather than
at the first conflicting write — a diagnostic that fires when two producers disagree has already let
a frame be wrong.

The test that proves it SHALL fail when the refusal is removed, and that SHALL be demonstrated rather
than asserted: M9's gate found that a criterion whose subject it never exercised passed 44 of 44 with
the enforcement point deleted.

#### Scenario: A second producer is refused by name
- **WHEN** a second producer registers for a field that already has one
- **THEN** registration SHALL fail naming the field and both producers, and the engine SHALL NOT
  start with two producers for one field

#### Scenario: The refusal is shown to be load-bearing
- **WHEN** the refusal is removed from the registration path
- **THEN** the test that claims it SHALL go red, and the demonstration SHALL be recorded with the
  change
