# editor-architecture Spec Delta

## ADDED Requirements

### Requirement: Terrain brushes are engine-evaluated, undoable and agent-reachable
The terrain editor SHALL author raise, lower, smooth, flatten, material-paint and hole strokes with
radius, strength and falloff, each completed stroke as one undoable modifier transaction. The
editor SHALL NOT compute terrain itself: after every change to the edited terrain's modifier stack,
undo and redo included, it SHALL send the stack to the engine's terrain service and SHALL show the
heights, material weights and holes the engine answers. Every stroke the panel can make SHALL also
be available to an agent as a command that records the same modifier, and the engine's last answer
SHALL be readable through a command.

#### Scenario: Undo sends the engine the stack as it was
- **WHEN** an author or an agent strokes the terrain and then undoes the stroke
- **THEN** the editor SHALL send the engine a stack byte-identical to the one before the stroke

#### Scenario: An agent's stroke is the panel's stroke
- **WHEN** an agent applies a brush with the same tool, points and brush settings as a panel gesture
- **THEN** the document SHALL record the same modifier kind and stroke payload

#### Scenario: A sculpt stroke with material layers present is accepted
- **WHEN** the terrain has a material layer and an author sculpts or cuts a hole
- **THEN** the stroke SHALL name no layer and SHALL be recorded
