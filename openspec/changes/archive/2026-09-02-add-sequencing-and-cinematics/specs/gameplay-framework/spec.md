## MODIFIED Requirements

### Requirement: One command stream
Gameplay intent SHALL be expressed as **gameplay commands**: reflected, schema-versioned semantic
operations — move, attack, build, use ability, interact — distinct from raw input events.

Commands SHALL be produced by **all producers through one path**: human input, artificial
intelligence, remote peers, replay playback, automated tests, and **sequences** (see
`sequencing-and-cinematics`). The simulation SHALL NOT be able to distinguish their origin.

Raw input SHALL NOT reach gameplay systems. Input actions (see `input-and-actions`) produce
commands; gameplay consumes commands. Likewise a sequence that changes authoritative state SHALL
emit commands rather than writing component data.

A command SHALL declare: its reliability, whether it may be predicted, whether it is local-only or
authoritative, and its validation requirements.

A command MAY carry **provenance** identifying its producer for diagnostics. Provenance SHALL NOT
affect validation, ordering, or execution.

Command submission SHALL scale: submission SHALL NOT serialise through a single lock, and commands
SHALL be accumulated per worker and committed deterministically.

#### Scenario: Origin is indistinguishable
- **WHEN** a move command arrives from a human, an AI, a network peer, and a replay
- **THEN** the simulation SHALL process all four identically

#### Scenario: Replay is intent, not state
- **WHEN** a session is replayed
- **THEN** it SHALL be reconstructed from the recorded command stream and the session seed

#### Scenario: A test drives a game
- **WHEN** an automated test issues commands
- **THEN** it SHALL use the same path as a player, with no test-specific gameplay entry point

#### Scenario: A cinematic is a producer, not an exception
- **WHEN** a sequence unlocks a door or starts an objective
- **THEN** it SHALL emit a command validated exactly as a player's would be, and provenance SHALL
  serve only diagnostics
