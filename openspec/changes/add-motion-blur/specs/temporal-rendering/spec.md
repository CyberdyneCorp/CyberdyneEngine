## ADDED Requirements

### Requirement: Per-object motion is derived from the previous frame's placement and vertices
The frame's depth prepass SHALL derive each surface's motion vector from the placement its instance
had last frame and the positions its vertices had last frame, both taken from what the renderer
already uploaded or stored, and SHALL NOT require any system to supply a motion of its own. Last
frame's placement SHALL be expressed about this frame's camera, so a still surface under a moving
camera carries the camera's motion alone. A surface with no history — an instance seen for the first
time, a slot handed to a new instance, or any surface after a history cut — SHALL carry camera motion
only rather than an invented one.

#### Scenario: A moving object's motion is its screen displacement
- **WHEN** an instance's placement changes between two frames and nothing else is told
- **THEN** every pixel of it SHALL carry a motion vector equal to the reprojection of its surface
  through its own displacement, and every pixel of the still world SHALL carry the camera's motion
  alone

#### Scenario: A deformed mesh moves by its stored previous vertices
- **WHEN** a mesh deformed on the device keeps last frame's vertices in the other half of a
  double-buffered output and its draw names them
- **THEN** its motion vectors SHALL be derived from those vertices, whichever half is current

#### Scenario: A still frame is unchanged
- **WHEN** nothing in the frame moves
- **THEN** the prepass's motion vectors, and the temporal resolve that reads them, SHALL be
  byte-identical to the frame before per-object motion existed

#### Scenario: Temporal antialiasing reprojects a moving object
- **WHEN** an object moves across a still background under temporal antialiasing
- **THEN** the resolve SHALL ghost less behind it than it does with camera motion alone
