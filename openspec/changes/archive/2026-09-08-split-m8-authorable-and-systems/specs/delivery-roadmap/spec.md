## ADDED Requirements

### Requirement: A milestone blocked by its own spike is two milestones
A milestone SHALL NOT couple work that is demonstrable and architecturally settled to work that
depends on its own named risk spike succeeding. Where it does, it SHALL be split, so that the settled
half can close, be demonstrated and be built upon while the risky half is still being designed.

The test is the closing artefact: **if a milestone's artefact cannot be reached without its risk
spike succeeding, and some other coherent artefact could be reached without it, the milestone
contains two.**

This is not a rule about size. A large milestone whose parts share one risk is one milestone; a small
one whose visible half waits on an unrelated unknown is two.

#### Scenario: A settled half is blocked by a risky half
- **WHEN** a milestone advances capabilities that are settled and demonstrable, and also capabilities
  that depend on an unproven shared abstraction
- **THEN** it SHALL be split so the settled capabilities close on their own artefact, and the split
  SHALL be recorded as an insertion rather than a renumbering

#### Scenario: Shared risk is not a split
- **WHEN** every part of a milestone depends on the same spike
- **THEN** it SHALL remain one milestone regardless of its size, because splitting it would produce
  two milestones that cannot close independently

### Requirement: An inserted milestone's rung is checked, not assumed
An insertion SHALL add its identifier to the ladder's ordered milestone list in position, and a test
SHALL assert that the milestone below it inherits its criteria and that it inherits theirs.

M5.5's insertion did not, and `criteria.rung` answered with the length of the list, sorting its ledger
to the end of the ladder. That was harmless only because nothing sat above it; the next milestone to
close would have inherited the wrong set in both directions.

#### Scenario: An insertion is ordered
- **WHEN** a milestone is inserted between two existing ones
- **THEN** the flattened evaluator SHALL place it between them, and a test SHALL fail if it does not
