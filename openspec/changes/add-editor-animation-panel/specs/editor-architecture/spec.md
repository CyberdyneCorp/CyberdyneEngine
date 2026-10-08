# editor-architecture Spec Delta

## ADDED Requirements

### Requirement: Animation graphs and clips editor
The animation graphs and clips editor SHALL author pose graphs on the shared graph canvas with the
engine's pose vocabulary and SHALL show a clip and its events on the shared timeline; it SHALL refuse to
edit until the engine has declared the vocabulary. Each edit — a node added, moved or removed, a wire
made or removed, a property set, an event placed, moved or removed — SHALL be one undoable transaction
and an agent tool that does the same thing. A graph SHALL be saved as the engine's canonical graph text.
The engine SHALL compile every change, and its diagnostics SHALL be shown on the node they name. The
preview SHALL be evaluated and drawn by the engine: playing, pausing, scrubbing and setting a parameter
SHALL be requests to the engine, the editor SHALL show the state, blend, time and events the engine
reports, and the editor SHALL NOT evaluate a pose.

#### Scenario: The previewed pose is the engine's evaluation
- **WHEN** an author scrubs a clip or the state machine to a time
- **THEN** the pose the engine previews SHALL equal, bit for bit, the program evaluated directly at that time

#### Scenario: An edited transition changes what the engine evaluates
- **WHEN** an author lengthens a transition's blend while the state machine is previewed
- **THEN** the preview SHALL be evaluated again from the edited graph, and the pose at the same time SHALL differ

#### Scenario: A cut is refused on its transition
- **WHEN** a transition's blend is zero
- **THEN** the engine's compile SHALL fail with a diagnostic on that transition, and the preview SHALL refuse the graph

#### Scenario: Events are placed on the timeline and undone
- **WHEN** an author places, drags and removes an event key on a clip
- **THEN** each SHALL be one undoable transaction on the graph's file, and the engine SHALL fire the event where it now is
