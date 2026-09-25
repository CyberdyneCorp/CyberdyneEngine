# Spec Delta

## ADDED Requirements

### Requirement: Live authored meshes in the viewport
The live editor viewport SHALL show the open world rendered by the engine, including the actual cooked geometry of each mesh instance at its authored translation, rotation, and scale. An empty world SHALL show an empty scene ready for placement. An unresolvable mesh reference SHALL produce a visible diagnostic instead of silently drawing substitute geometry.

#### Scenario: Import and manipulate an FBX
- **WHEN** a user imports an FBX from outside the project and places its cooked mesh in an empty world
- **THEN** the viewport SHALL show that mesh with its base-colour texture, including a texture embedded in the FBX, and Move, Rotate, and Scale edits SHALL update its visible placement.

#### Scenario: Import before the first save
- **WHEN** a user places an imported mesh in a new empty world whose file has no component declarations yet
- **THEN** the live runtime SHALL receive the newly authored declarations and render the textured mesh before the world is saved or reopened.

#### Scenario: Save and reopen an authored scene
- **WHEN** a user saves a scene containing an imported FBX after changing its placement and reopens the project and scene
- **THEN** the viewport SHALL restore the same mesh, texture, and authored translation, rotation, and scale.

#### Scenario: Empty scene
- **WHEN** a new empty project opens without authored objects
- **THEN** the viewport SHALL show no proxy objects and SHALL remain ready to place an asset.

#### Scenario: Pick the drawn mesh
- **WHEN** a user picks a point on a placed non-box mesh
- **THEN** the selected identity and transform gizmo SHALL refer to that drawn instance.

### Requirement: Continuous transform preview
The engine viewport SHALL render the current translation, rotation, or scale while a gizmo drag is in progress. Releasing the pointer SHALL commit one undoable transaction; cancelling SHALL restore the starting transform in both the document and engine viewport.

#### Scenario: Drag a transform handle
- **WHEN** a user holds the pointer and drags a Move, Rotate, or Scale handle
- **THEN** the object and numeric transform fields SHALL update before pointer release.

#### Scenario: Cancel a transform drag
- **WHEN** a user cancels an in-progress transform drag
- **THEN** the engine viewport SHALL return to the pre-drag placement without an undo entry.

### Requirement: Compact viewport chrome
The viewport's transform fields SHALL align by column across Position, Rotation, and Scale rows. The application menu header SHALL show the product mark without the horizontal wordmark. The orientation widget's background SHALL be translucent enough for the scene to remain visible behind it.

#### Scenario: Inspect the editor with an object selected
- **WHEN** a user selects an object and reads the viewport controls
- **THEN** the three transform rows SHALL have shared numeric columns, the header SHALL show only the product mark, and the scene SHALL remain visible behind the orientation widget.
