## MODIFIED Requirements

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
