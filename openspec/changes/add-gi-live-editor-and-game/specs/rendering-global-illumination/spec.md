## ADDED Requirements

### Requirement: Device GI as a frame feature
The engine SHALL provide the device global illumination as one feature a host attaches to its
assembled frame over an illumination system the host already runs: the host's update decides
placement, budgets, settings, volumes, classes and emitter rules, and the frame resolves the
system's caches on the device and composites them where the ambient sits. A host SHALL be able to
turn the feature on and off from one frame to the next without recreating the frame's pipelines.

A frame without the feature attached, or with it attached under a mode that admits no dynamic
bounce into any surface, SHALL be the frame without it, byte for byte.

#### Scenario: A host changes the GI between two frames
- **WHEN** a host puts an emitter out through an emitter rule between two frames
- **THEN** the frames after it SHALL be lit without that emitter's light, nothing rebuilt

#### Scenario: The GI switched off
- **WHEN** the system's mode is set to one that admits no dynamic bounce
- **THEN** the frame SHALL composite none of the GI's targets

### Requirement: GI debug views on the device
Every GI debug view SHALL be drawable by the device in the frame it describes, from the device
frame's own state — its surfaces, its published indirect light, its reprojection history, the probe
set, the surface cache and its lookup, the distance field — at the frame's resolution and
interactive rates. Each view SHALL be the host's view of the same datum over the frame's surfaces,
within a stated bound; a view whose data the device does not hold SHALL say so, so the caller draws
it on the host.

#### Scenario: The indirect diffuse view in the editor
- **WHEN** an author selects the indirect diffuse view on a device that runs the GI
- **THEN** the view SHALL be drawn by the device from the targets the frame composites, and a frame
  with it SHALL take tens of milliseconds at most rather than hundreds

### Requirement: GI in the editor viewport and in a game's frame
The editor's viewport SHALL light its Lit view with the device GI of the authored world when the
world's GI settings admit it, and every document change to the GI settings, volumes, classes or
emitter rules SHALL reach the running GI in place. A game host SHALL be able to light its frame with
the same feature over the system its game tunes through the native ABI, and a Swift game running in
the editor's Play SHALL reach the GI the viewport is lit by.

#### Scenario: An author lowers the GI quality
- **WHEN** the author sets the world's GI quality to low
- **THEN** the viewport's next frames SHALL be lit at the new quality, and nothing SHALL be rebuilt

#### Scenario: A Swift game puts a lamp out
- **WHEN** a Swift game switches a lamp's material off through the ABI
- **THEN** the game's frames after it SHALL be lit without the lamp's light, and lighting it again
  SHALL bring the light back
