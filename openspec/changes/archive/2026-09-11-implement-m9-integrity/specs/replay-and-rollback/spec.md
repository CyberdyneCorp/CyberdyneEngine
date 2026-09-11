## ADDED Requirements

### Requirement: The log's readers are enumerated, and that they read one record is checked
`replay-and-rollback` requires that "Replay, rollback, and lockstep SHALL share one command log" and
that "A second representation of participant intent SHALL NOT exist". **It names three readers and
the engine has five**, because the crash replay buffer and the divergence validator read the same
records and neither appears in that list — and `networking-and-replication` describes replication's
inputs without ever saying they are the same log.

A requirement stated over three of five readers is a requirement two readers may drift out of
without contradicting it. The engine SHALL therefore enumerate every reader of the command log, and
the claim that they share one record type SHALL be checked by a test rather than by review.

The five readers are: playback and seeking, rollback re-simulation, replication's input path, the
crash replay buffer, and the divergence validator.

#### Scenario: A reader with its own record type is a defect a check finds
- **WHEN** a reader of the command log declares a record type of its own rather than the shared one
- **THEN** a check SHALL fail naming that reader, rather than the divergence appearing later as a
  replay that does not reproduce a networked session

#### Scenario: A new reader is declared
- **WHEN** a sixth reader of the command log is added
- **THEN** it SHALL be declared in the enumeration, and the check SHALL fail until it is

### Requirement: The duplicate-effect proof is a negative control
`replay-and-rollback` requires a side-effect ledger so that rollback "re-simulates without
re-applying ledgered side effects", and `docs/ROADMAP.md` makes the proof of it an exit criterion.

The test that proves it SHALL fail when the ledger is removed, and that SHALL be demonstrated rather
than asserted — for the same reason `vfx-system`'s firewall delta requires it at M8.c: a test that
still passes when its subject is deleted is a test of nothing, and this project has shipped criteria
that never ran.

#### Scenario: The ledger is removed
- **WHEN** the side-effect ledger is disabled
- **THEN** the duplicate-effect test SHALL fail, and the demonstration SHALL be recorded
