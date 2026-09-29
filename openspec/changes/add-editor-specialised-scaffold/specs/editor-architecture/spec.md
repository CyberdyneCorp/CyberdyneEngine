# editor-architecture Spec Delta

## ADDED Requirements

### Requirement: Specialised editors share one panel scaffold
Every specialised editor panel SHALL be drawn through one shared scaffold that opens its domain
through the specialised-editor host, shows a standard header with undo and redo over the document's
transaction history, and shows the editor's refusals in a diagnostics area rather than only on its
canvas. Every command a specialised editor panel invokes SHALL be registered, SHALL be exposed as an
agent tool without exclusion, and SHALL be either a read or a reversible mutation, or — declared by
the editor as such — a long operation whose only effect is outside every document (an external
effect, such as a bake writing a cooked file); the editor SHALL refuse to start, naming the command,
when one is not.

#### Scenario: A panel command an agent cannot reach is refused
- **WHEN** a specialised editor's panel invokes a command that is unregistered, excluded from agents, or irreversible
- **THEN** registering the editor SHALL fail and name that command

#### Scenario: The header undoes the editor's last transaction
- **WHEN** an author presses Undo in a specialised editor's header
- **THEN** the editor SHALL invoke the same undo command as the Undo History panel

### Requirement: Graph and timeline editors share one view each
Every node-graph editor SHALL draw through one shared canvas view with a palette that hosts the
domain's engine-declared node types. Every keyed-time editor SHALL draw through one shared timeline
view that reads the timeline surface without mutating it. Each completed timeline gesture SHALL be
reported as one edit that answers its own inverse, and Escape during a drag SHALL cancel it and
record nothing.

#### Scenario: A declared vocabulary becomes a palette
- **WHEN** a graph domain declares node types
- **THEN** its palette SHALL offer exactly those types, and a type picked from it SHALL be placed on the shared canvas

#### Scenario: A key drag is one edit
- **WHEN** an author drags a key across many frames and releases it
- **THEN** the timeline SHALL report one move, and applying that move's inverse SHALL restore the track
