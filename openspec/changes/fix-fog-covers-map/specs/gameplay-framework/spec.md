## MODIFIED Requirements

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
state as the same session with no picture. The fog of war's grid SHALL cover the whole map, sized
from the cooked map's bounds, so that ground the player's team never saw is drawn never explored
wherever it lies on the map, and a unit of another team is drawn only where that team's sight
shows it. The frame drawn on it SHALL have a stated budget, measured on a named device and held by
a test.

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
- **THEN** the two SHALL report the same game, the lockstep company's digest included, and every
  fog of war decision SHALL be the one the session makes with no map

#### Scenario: The fog covers the map
- **WHEN** the session runs on the map
- **THEN** the fog of war's grid SHALL contain the cooked terrain's bounds and the playing square,
  and a unit of another team far from the player's sight SHALL not be drawn

#### Scenario: The frame keeps its budget
- **WHEN** the session is drawn on the map at 1920x1080 on the device the budget names
- **THEN** a frame SHALL take no more than the budget on average
