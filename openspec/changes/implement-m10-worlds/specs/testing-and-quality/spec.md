## ADDED Requirements

### Requirement: A declared test kind exists in the tree, and a check says so
The taxonomy names seven kinds of test and gives each a location and a budget. `tests/determinism/`
has held a README and no test since M0, and `tests/CMakeLists.txt` refuses an unknown kind with the
words "determinism joins them at M9". M9 shipped a determinism module, a validator, a lint and two
hundred cases — in `unit` and `integration` — and left the declared suite empty.

A kind that is declared in the taxonomy and absent from the tree is a budget nobody pays and a
location nobody reads. The engine SHALL therefore check that **every kind the taxonomy declares has
at least one registered suite**, and the check SHALL name the kind and its location when it does not.

A kind that is deliberately empty until a later milestone SHALL be declared as such in one place, so
that "not yet" is a recorded decision with a rung rather than the absence of a check.

#### Scenario: A declared kind with no suite is a finding
- **WHEN** the taxonomy declares a kind whose location holds no registered suite
- **THEN** the check SHALL fail naming the kind, its location and the milestone at which it was due

#### Scenario: The determinism suite carries the cases the specification names
- **WHEN** the determinism suite exists
- **THEN** it SHALL hold the golden replays, the replay and save fuzzing and the transactional save
  tests this specification requires of it, rather than a placeholder that satisfies the check
