## ADDED Requirements

### Requirement: Retired instance slots fit what they are reused for
A simulation world SHALL reuse a retired effect instance's slot only for a system whose every
emitter's capacity fits the alive flags the slot's blocks were made with and whose parameters fit
the slot's parameter words; any other system SHALL be given a new slot. No instance's alive flags or
parameters SHALL overlap another's.

#### Scenario: A larger system after a smaller one retired
- **WHEN** a small system's instance retires and a larger system with the same number of emitters is played beside a live instance
- **THEN** the larger system's alive flags SHALL not overlap the live instance's, and the small system SHALL still reuse the retired slot
