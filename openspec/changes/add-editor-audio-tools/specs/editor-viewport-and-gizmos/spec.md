# editor-viewport-and-gizmos Spec Delta

## ADDED Requirements

### Requirement: Spatial audio preview
The editor view SHALL show every enabled audio source in the world as an engine-drawn handle with
its full-volume radius and its silence radius, distinguishable by line style as well as colour and
projected with the view that rendered the frame. A source's handle SHALL be selectable like a light
handle, its radii SHALL be editable as one undoable transaction, and previewing it SHALL play its
cue in the engine at its position, heard from the viewport camera. Game view SHALL omit the
handles.

#### Scenario: The radii are where the engine attenuates
- **WHEN** a source's maximum distance is changed
- **THEN** the dashed ring SHALL move to the new radius, and a preview from outside it SHALL be silent

#### Scenario: A nearer source is louder
- **WHEN** the same cue is previewed from a source nearer the camera
- **THEN** the engine SHALL report a higher gain and mix it louder
