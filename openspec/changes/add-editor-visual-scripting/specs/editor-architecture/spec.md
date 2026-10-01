# editor-architecture Spec Delta

## ADDED Requirements

### Requirement: Gameplay and utility graphs editor
The gameplay and utility graphs editor SHALL author gameplay graphs on the shared graph canvas with the
engine's node vocabulary, and SHALL refuse to edit until the engine has declared it. A new graph SHALL
answer an event; the palette SHALL NOT offer a per-frame entry point. Each edit — a node added, moved or
removed, a wire made or removed, a property set — SHALL be one undoable transaction and an agent tool
that does the same thing. A graph SHALL be saved as the engine's canonical graph text and SHALL be
attachable to an entity as one undoable transaction. The engine SHALL compile every change, and its
diagnostics SHALL be shown on the node they name. During play the editor SHALL raise events on an
entity's graphs and SHALL show what the engine's compiled program did; the editor SHALL NOT interpret a
graph.

#### Scenario: An authored graph is the engine's text
- **WHEN** an author or an agent builds a graph node by node
- **THEN** the saved file SHALL be byte for byte the text the engine writes for that graph, and the engine SHALL compile it

#### Scenario: A diagnostic names its node
- **WHEN** a graph names a function the engine does not declare
- **THEN** the engine's diagnostic SHALL name that node and the function, and the editor SHALL mark the node and select it from the diagnostic

#### Scenario: A graph runs in play as its Swift twin does
- **WHEN** a unit's graph receives a move order in play
- **THEN** the unit SHALL move and play its arrival cue as a Swift behaviour written for the same order does, on the same ticks

#### Scenario: Undo restores the graph
- **WHEN** an author undoes graph edits
- **THEN** the graph file SHALL return to each earlier text, and undoing its creation SHALL remove it
