## ADDED Requirements

### Requirement: A played lockstep session replays from its command log
A lockstep session a person played SHALL be reproducible from its command log alone — every order,
the player's and an opponent's, with the tick it was given for and its raw fixed-point target — so
that recording the logged orders into a fresh session on their ticks, with no player and no
opponent, ends in the digest the played session ended in. A log whose one order differs SHALL end in
a different digest.

#### Scenario: The same digest
- **WHEN** a session played by a select-and-attack and an opponent is recorded and its log replayed
- **THEN** the replay SHALL end in the recorded issuer and follower digests after the recorded number of ticks

#### Scenario: A moved order
- **WHEN** one order's target in the log is moved by the smallest fixed-point step and replayed
- **THEN** the replay SHALL end in another digest
