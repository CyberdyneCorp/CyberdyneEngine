## MODIFIED Requirements

### Requirement: Determinism lint
Systems declared deterministic SHALL be checkable statically. The build SHALL be able to report, for
such systems: use of wall-clock time, use of an ambient random generator, iteration over containers
with unspecified order used as a decision order, reads of presentation-classified data, and use of
floating-point operations disallowed by the active profile.

Findings SHALL name the system and the source location, and SHALL be configurable as errors or
warnings.

#### Scenario: A wall-clock read is flagged
- **WHEN** a deterministic system reads the system clock
- **THEN** the lint SHALL report it with its location

#### Scenario: A finding names its system
- **WHEN** the lint reports a finding in a source compiled under a declared determinism profile
- **THEN** the finding SHALL name the system that declared the profile as well as the file and line,
  and the lint's selftest SHALL fail if it does not
