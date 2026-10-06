# animation-and-skinning Spec Delta

## ADDED Requirements

### Requirement: Requested crossfades and triggers
A host SHALL be able to request any state of an instance's program with a crossfade duration,
whatever the program's own transitions, or cut to it with a zero duration; the requested blend SHALL
run to completion before the program's transitions are considered again, and the entered state's
clips SHALL start from zero. A host SHALL be able to raise a trigger parameter that reads 1 for
exactly the next tick's advance and 0 after it.

#### Scenario: Play a state the program has no edge to
- **WHEN** an idle instance is asked to play `run` over 0.25 s and the program has no idle-to-run edge
- **THEN** it SHALL blend to run over 0.25 s from the clip's first frame, and a program transition
  requested during the blend SHALL NOT interrupt it

#### Scenario: A trigger fires once
- **WHEN** a trigger that opens the walk edge is fired before a frame of three ticks
- **THEN** the first tick SHALL take the edge and the trigger SHALL read 0 before the second
