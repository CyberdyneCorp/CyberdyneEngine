## MODIFIED Requirements

### Requirement: Overlays and in-viewport interfaces
The viewport SHALL support overlays — measurement, statistics, safe frames, composition guides,
camera information, coordinate readouts, and object labels — which SHALL be individually toggleable
and SHALL not be baked into the rendered image used for judging appearance.

Overlays SHALL be excluded from any capture intended to represent the shipping image, and a capture
SHALL state whether overlays were included.

In-viewport controls SHALL not obstruct the content being judged, and SHALL be dismissible.

Viewport **controls** — projection and rendering mode, show flags, camera speed, snapping,
transform mode, maximise, and debug visualisation selection — SHALL likewise be presented as
overlays within the viewport rather than as an additional toolbar occupying vertical space above
it. Their visual treatment is specified in `editor-visual-language`.

The **view-orientation widget** is an overlay, not a manipulator: it presents camera and world
orientation, SHALL show only the three principal axes, and SHALL NOT carry rotation rings, scale
handles, or translation arrows that would make it read as a transform gizmo.

#### Scenario: Controls do not consume viewport height
- **WHEN** viewport controls are added or extended
- **THEN** they SHALL be placed as overlays, and the rendered viewport area SHALL NOT shrink

#### Scenario: Orientation is not manipulation
- **WHEN** a user drags the view-orientation widget
- **THEN** the camera SHALL orient, and no object transform SHALL change

#### Scenario: A capture is clean
- **WHEN** the user captures a viewport image for reference
- **THEN** overlays SHALL be excluded unless explicitly requested
