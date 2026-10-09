# editor-architecture Spec Delta

## ADDED Requirements

### Requirement: Animation preview on the project's own characters
The animation editor SHALL preview a graph on a character the project imported as well as on the
engine's built-in one. A project character SHALL be a model whose import cooked a skeleton, with the
clips the project's imports cooked for that skeleton. A clip SHALL be named by its sub-asset's name.
The engine SHALL load the character's cooked records and SHALL refuse, by name, a clip cooked for
another skeleton. Choosing a graph's character SHALL be one undoable transaction and an agent tool
that does the same thing. The preview SHALL be evaluated on the chosen character, and the clips a
clip node can name SHALL be that character's.

#### Scenario: An imported character previews as its clip evaluates
- **WHEN** an author previews an imported character's clip at a time
- **THEN** the pose the engine previews SHALL equal, bit for bit, the importer's cooked clip sampled directly at that time

#### Scenario: A clip cooked for another skeleton is refused
- **WHEN** a project's clip moves joints the chosen character's skeleton does not have
- **THEN** the engine SHALL refuse that clip by name and SHALL play the character's other clips

#### Scenario: Choosing a character is undone
- **WHEN** an author chooses an imported character for a graph and then undoes it
- **THEN** the graph SHALL play on the built-in character again, and the engine SHALL be sent the built-in character before the next preview

### Requirement: Baking an animation graph for the game
The animation editor SHALL bake a graph for its project character into the rig a game loads: the
compiled program and every clip the program names, with the events authored on the timeline in place
of the clip's own. The engine SHALL compile and cook the rig. The editor SHALL write the files the
engine returns, replacing the rig's previous bake. A graph with an error SHALL bake nothing, and the
editor SHALL show why. The built-in character SHALL NOT be baked. In the editor's Play, every baked
rig SHALL be registered under its graph's name, so a script can attach an animator to it and receive
its events in its frame callback.

#### Scenario: An event placed in the editor fires in Play
- **WHEN** an author places an event on a clip's timeline, bakes the graph, and a Swift behaviour attaches to the rig in Play
- **THEN** the behaviour's frame callback SHALL receive the event on the fixed tick in which its authored time falls

#### Scenario: A bake writes the rig the engine cooked
- **WHEN** an agent bakes a graph over MCP
- **THEN** the files the engine answers SHALL be written under the rig's directory, and a file from an earlier bake that the new one does not name SHALL be gone
