## ADDED Requirements

### Requirement: The engine is proved by a consumer, not only by its own suites
A capability recorded Complete SHALL have been exercised by a consumer outside its own test suite
before 1.0 is claimed to be proven. The vertical slice at `samples/12-rts/` is that consumer, and a
row it contradicts SHALL be recorded as a finding against the row rather than worked around inside
the sample.

This rule exists because sixty capabilities reached Working verified one at a time, and eleven
checks have shipped in this repository unable to fail. A row observed only by the suite written
alongside it is a row observed once.

#### Scenario: The slice contradicts a row recorded Complete
- **WHEN** the vertical slice cannot do what a Complete row's specification requires
- **THEN** the finding is recorded against that row with the requirement it contradicts
- **AND** the row's tier is revisited rather than the sample carrying a workaround

#### Scenario: The slice finds a defect
- **WHEN** a defect is found while building the slice
- **THEN** it is repaired in a change carrying a test that fails without the repair
- **AND** the register records the defect, its row, its test, and the tier the row held when found
