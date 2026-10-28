## ADDED Requirements

### Requirement: Combat in the lockstep path through the ABI
ABI 1.16 SHALL append `lockstep_arm`, `lockstep_fortify`, `lockstep_combatant`,
`lockstep_structure`, `lockstep_events` and `lockstep_projectiles`, and the order kinds
`CY_LOCKSTEP_ORDER_ATTACK_MOVE` and `CY_LOCKSTEP_ORDER_ATTACK`. Arming a unit and fortifying a
structure are session configuration: both SHALL be accepted only with no phase set and before the
session's first tick, and refused with `CY_RESULT_PERMISSION_DENIED` after it. Reading a combatant,
a structure, the events and the projectiles SHALL be allowed in every phase. Events SHALL be numbered
from zero in the order the session made them, and `lockstep_events` SHALL hand out the events
numbered from a caller's cursor, oldest first, from a bounded history. A backend without combat
SHALL answer `CY_RESULT_UNSUPPORTED` from the combat entries, and no bound session
`CY_RESULT_UNAVAILABLE`. The Swift overlay SHALL expose the entries through CyberdyneKit's
`Lockstep`.

#### Scenario: A battle read from a cursor
- **WHEN** a game reads `lockstep_events` from a cursor after every tick, keeping the number after the last event it read
- **THEN** it SHALL read every event the session made exactly once, numbered without a gap

#### Scenario: Arming is configuration
- **WHEN** a game arms a unit in a fixed or a frame update, or after the first tick
- **THEN** the call SHALL return `CY_RESULT_PERMISSION_DENIED` and arm nothing

#### Scenario: A history that is outrun
- **WHEN** a reader falls further behind than the history holds
- **THEN** it SHALL be handed the most recent events, oldest first, their numbers showing what it missed
