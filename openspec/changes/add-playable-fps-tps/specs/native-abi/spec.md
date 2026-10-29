## ADDED Requirements

### Requirement: The pointer's motion in a frame
The pointer a game reads in a frame update SHALL report its motion in window pixels with +y down,
the space its position is in, and SHALL report the motion made since the previous frame update
whether or not a fixed tick ran in between — each event counted once, however often the pending
events are observed before a tick resolves them.

#### Scenario: A mouse pushed away
- **WHEN** a mouse is pushed away from the player
- **THEN** the pointer's y motion SHALL be negative

#### Scenario: A frame with no tick
- **WHEN** the mouse moves during a frame in which no fixed tick runs
- **THEN** that frame's pointer SHALL report the motion, and the tick that later resolves it SHALL
  not report it again
