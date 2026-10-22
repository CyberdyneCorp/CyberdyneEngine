## MODIFIED Requirements

### Requirement: Fog of war rendering
A frame SHALL be able to draw one team's fog of war as a stage of its own, declared by its
producer, on linear HDR after the temporal stage and before depth of field, motion blur and bloom.
The producer SHALL read the team's grid without writing it, keep a visibility texture of one texel
a cell faded toward the grid's state on the frame's clock — a cell revealed brightening over a
reveal time and one left behind darkening over a hide time — upload it in the frame, and apply it
to every surface the depth reaches: a visible surface SHALL be written exactly as it was read, an
explored one desaturated and darkened, and a never-explored one the unexplored colour. The texture
SHALL be sampled smoothly, so that a boundary between cells is a ramp rather than a step. A surface
off the grid SHALL be treated as never explored, whatever the cells at the grid's edge hold, and
never as visible. Units of other teams SHALL be drawn as the team's sight of them decides: not at
all when hidden, and where last seen when a ghost. The same field SHALL be available as the minimap's overlay, so the minimap
and the frame agree about where the fog is. With the stage off the frame SHALL be the frame without
it, byte for byte.

#### Scenario: Off is the frame before
- **WHEN** a frame is drawn without the fog of war stage
- **THEN** it SHALL be byte-identical to the frame drawn before the stage existed

#### Scenario: A team that sees everything changes nothing
- **WHEN** the stage runs with every cell visible
- **THEN** the frame SHALL be byte-identical to the frame without the stage

#### Scenario: The texture is the grid
- **WHEN** the stage uploads a settled field
- **THEN** the texture on the device SHALL hold the grid's state at every cell

#### Scenario: Explored and unexplored ground
- **WHEN** the stage runs over ground with explored and never-explored cells
- **THEN** explored ground SHALL be darker and greyer than without the fog, never-explored ground
  SHALL be the unexplored colour, and every pixel SHALL be the host reference's shading of the
  colour it read at the world position its depth reconstructs

#### Scenario: Smooth edges
- **WHEN** a boundary between visible and unexplored cells is drawn
- **THEN** no two neighbouring pixels across it SHALL differ by more than a quarter of the contrast
  the fog spans there

#### Scenario: Enemy units by sight
- **WHEN** an enemy unit stands outside the team's vision on explored ground
- **THEN** it SHALL not be drawn, and a structure the team remembers SHALL be drawn where it was
  last seen

#### Scenario: Ground off the grid
- **WHEN** the stage runs over ground that lies outside the grid, beside edge cells the team sees
- **THEN** that ground SHALL be the unexplored colour, and the minimap SHALL cover it as never
  explored

#### Scenario: The minimap shows the same fog
- **WHEN** a HUD draws the minimap with the fog's overlay
- **THEN** the minimap SHALL be unchanged where the field is visible, darker where it is explored,
  and black where it was never explored
