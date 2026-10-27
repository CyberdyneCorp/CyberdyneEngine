## ADDED Requirements

### Requirement: A third-person demo
The project SHALL ship a rendered third-person demo whose gameplay is written in Swift: a skinned
character moved by a capsule character controller, animated through a locomotion blend space and
foot placement, that walks, runs, jumps and climbs stairs and slopes in a cooked level; a camera
whose framing the game asks for and whose collision the physics world answers; and footsteps and
ambient sound. The host SHALL carry no gameplay decision. A scripted run SHALL be reproducible —
two runs reporting the same digest of every frame's character, camera and pose — and SHALL be drawn
by the engine's frame with shadows, ambient occlusion, global illumination and temporal upscaling
within a stated budget measured on a named device and held by a test, its gameplay the same as the
run with no picture.

#### Scenario: The scripted run is reproducible
- **WHEN** the demo's scripted run is run twice headless
- **THEN** the two SHALL print the same report and the same digest

#### Scenario: Drawing does not change the game
- **WHEN** the scripted run is drawn and run with no picture
- **THEN** the two SHALL report the same game

#### Scenario: The frame keeps its budget
- **WHEN** the run is drawn at 1920x1080 on the device the budget names
- **THEN** a frame SHALL take no more than the budget on average

#### Scenario: The negative control
- **WHEN** the demo runs with no game behaviours
- **THEN** every host path SHALL run and nothing SHALL move, sound or collide
