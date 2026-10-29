## ADDED Requirements

### Requirement: A game asks its host to hold or end the session
A game SHALL be able to ask the host that runs it to hold the simulation — frames running with no
fixed step, input still resolved so the request can be withdrawn — and to end the session, through
state the game writes and the host reads after each frame; the decision SHALL be the game's and the
host SHALL act on it from the next frame. A held frame SHALL report the simulation as paused.

#### Scenario: Pause and quit
- **WHEN** a player pauses, resumes, opens the menu, resumes from it and quits from it
- **THEN** the host SHALL run no fixed step in the frames the game held, SHALL run one in every other, and SHALL end the session after the frame the game asked it to
