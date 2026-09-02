## MODIFIED Requirements

### Requirement: Precision
Runtime math SHALL use 32-bit `float` for transforms, geometry, and rendering. `double` SHALL be
used only where explicitly required: the simulation clock, accumulated time, geodetic and
interchange coordinate helpers, and analysis tooling.

Large-world support SHALL be provided by **cell-relative coordinates** and **camera-relative
rendering** — positions are stored relative to a spatial cell and rebased to the camera before
submission — rather than by making the whole engine double precision.

The authoritative persistent form of a world position SHALL be cell-relative (see
`world-partition-and-streaming`): a cell coordinate plus a 32-bit local offset. Because the local
offset is bounded by cell size, 32-bit precision is sufficient at any distance from the world
origin. A 64-bit accessor exists for tooling and interchange and is not the runtime
representation.

#### Scenario: Distant object stays stable
- **WHEN** an object is 100 km from the world origin
- **THEN** its cell-relative position SHALL be exact in 32-bit floats, and camera-relative
  rebasing SHALL keep its rendered transform free of visible jitter

#### Scenario: Time accumulation
- **WHEN** the main loop accumulates elapsed time
- **THEN** the accumulator SHALL be `double` so drift does not accumulate over long sessions

#### Scenario: Double precision does not spread
- **WHEN** a subsystem is tempted to store global double-precision positions
- **THEN** the cell-relative form SHALL be used instead, so precision handling stays confined to
  the world layer
