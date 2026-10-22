## ADDED Requirements

### Requirement: A strategy test map
The project SHALL ship a test map for its strategy sample that composes the engine's terrain,
painted terrain layers, procedural foliage placement, standard materials, skinned animated units,
sun shadows, ambient occlusion, global illumination and fog of war into one place. The map SHALL be
authored as a text source naming engine objects — terrain modifiers, layers, species, placement
rules and placements — and SHALL be cooked by a deterministic cook into a content package, two cooks
of one source writing byte-identical packages. A host SHALL load the map from the package by asset
identity through the asset system, refusing an asset of the wrong kind, a damaged payload, a newer
format and a truncated payload with the reason. The map SHALL keep the strategy sample's playing
square flat and SHALL be presentation only: a session drawn on it SHALL reach the same simulation
state as the same session with no picture. The frame drawn on it SHALL have a stated budget,
measured on a named device and held by a test.

#### Scenario: The cook is deterministic
- **WHEN** the map's source is cooked twice
- **THEN** the two packages SHALL be byte-identical, and the package SHALL load back to the content
  that was cooked

#### Scenario: A damaged asset is refused
- **WHEN** a payload in the package is changed, loaded as another kind, declares a newer format or
  is cut short
- **THEN** the load SHALL fail and say which, rather than decode it

#### Scenario: The map does not change the game
- **WHEN** the strategy sample's scripted session runs drawn on the map and runs with no picture
- **THEN** the two SHALL report the same game, the lockstep company's digest included

#### Scenario: The frame keeps its budget
- **WHEN** the session is drawn on the map at 1920x1080 on the device the budget names
- **THEN** a frame SHALL take no more than the budget on average
