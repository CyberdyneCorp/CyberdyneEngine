# editor-architecture Spec Delta

## ADDED Requirements

### Requirement: Gameplay graph debugger and hot reload in the editor
The gameplay graph editor SHALL debug the engine's running graphs during play: a breakpoint gutter on each
node, the node play is paused before marked, the most recently executed nodes highlighted, pause,
continue, step over and step into controls, and a watch list showing the inspected entity's graph
variables and watched pins. Breakpoints set before play SHALL take effect when play starts. Each of these
SHALL be a command and an agent tool of the same name. Saving a graph that play runs, and undoing or
redoing an edit to it, SHALL reload it in play, and the editor SHALL show what the reload kept or the
engine's refusal on its node. The editor SHALL NOT interpret a graph.

#### Scenario: An agent debugs a graph as a person does
- **WHEN** an agent sets a breakpoint before play, then steps, continues and reads a watch over its tools
- **THEN** the engine SHALL receive the breakpoint when play starts and each control in order, and the tool results SHALL report the paused node and the watched values

#### Scenario: Saving a running graph reloads it
- **WHEN** a graph play runs is edited, and the edit is undone
- **THEN** the engine SHALL be sent the new text and then the restored text to reload
