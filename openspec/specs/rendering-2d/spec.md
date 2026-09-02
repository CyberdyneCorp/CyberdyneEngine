# rendering-2d Specification

## Purpose

Defines 2D rendering: sprites, tilemaps, 2D lighting and shadows, batching, and the relationship
between the 2D and 3D pipelines. 2D is a first-class path, not an afterthought layered on 3D.

## Requirements

### Requirement: 2D is a distinct pipeline sharing infrastructure
2D rendering SHALL use the same RHI, render graph, shader system, and material model as 3D, but a
distinct pipeline optimised for sorted, batched quad and mesh submission.

2D content SHALL be renderable: standalone (a purely 2D project), composited over 3D (a HUD), and
embedded in 3D (a world-space canvas).

#### Scenario: Pure 2D project
- **WHEN** a project contains no 3D content
- **THEN** the 3D passes SHALL be absent from the render graph entirely

#### Scenario: 2D over 3D
- **WHEN** a HUD is drawn over a 3D scene
- **THEN** it SHALL be composited after tonemapping by default, so UI colours are not affected by
  exposure and grading

### Requirement: Sprite and 2D primitives
The engine SHALL provide 2D renderable components: `Sprite` (a textured quad with a source rect,
pivot, flip, and modulate), `NineSlice`, `TiledSprite`, `Line2D` (a polyline with joint and cap
modes and width curve), `Polygon2D` (a triangulated polygon with UVs and vertex colours),
`Mesh2D`, and `AnimatedSprite` (frame sequences with per-frame duration).

Each SHALL support: a material, a sort key, a layer, tint, and texture filter and wrap overrides.

#### Scenario: Sprite from an atlas
- **WHEN** a sprite references a region of an atlas texture
- **THEN** it SHALL sample that region with correct filtering, with padding preventing bleed from
  neighbouring regions

### Requirement: Draw ordering
2D draw order SHALL be determined by an explicit sort key composed of: layer, sort order within
the layer, an optional Y-sort value, and a stable tiebreak.

**Y-sorting** SHALL be supported per layer, ordering by world Y so entities lower on screen draw
in front.

#### Scenario: Y-sorted top-down scene
- **WHEN** a layer enables Y-sorting
- **THEN** entities lower on screen SHALL be drawn later, appearing in front, updated as they move

#### Scenario: Deterministic ties
- **WHEN** two sprites have identical sort keys
- **THEN** the tiebreak SHALL be stable across frames so they do not flicker

### Requirement: Batching
Consecutive draws sharing a pipeline, material, and texture set SHALL be merged into a single
instanced draw, with per-instance data (transform, source rect, colour, custom values) written to
a per-frame instance buffer.

A batch SHALL break on: pipeline change, material change, texture set change, scissor or clip
change, or a render target change.

The engine SHALL report batch counts and break reasons so content can be organised to batch well.

#### Scenario: Thousands of sprites, few draws
- **WHEN** 5 000 sprites share an atlas and material
- **THEN** they SHALL be submitted as a small number of instanced draws

#### Scenario: Batch break diagnosis
- **WHEN** a 2D scene has an unexpectedly high draw count
- **THEN** the diagnostic SHALL report the dominant break reason

### Requirement: Tilemaps
The engine SHALL provide a tilemap system with:

- **Tilesets** — a texture atlas plus per-tile data: collision shapes, navigation regions,
  occlusion shapes, custom typed data, animation frames, and terrain membership
- **Tilemap layers** — sparse chunked cell storage, each cell referencing a tile and a variant
  (flip, rotate, alternative)
- **Grid shapes** — square, isometric, hexagonal (flat- and pointy-topped), and half-offset
- **Chunked rendering** — cells grouped into chunks batched as one draw and rebuilt only when
  dirty
- **Terrain autotiling** — constraint-based selection matching tiles by corner and edge
  membership, resolved when cells are painted
- **Runtime cell overrides** — per-cell modifications without editing the tileset

Physics, navigation, and occlusion data SHALL be generated per chunk and registered with the
respective servers.

#### Scenario: Single cell edit
- **WHEN** one cell changes
- **THEN** only its chunk's geometry, collision, and navigation SHALL be rebuilt

#### Scenario: Terrain painting
- **WHEN** cells are painted with a terrain in corner-and-edge mode
- **THEN** the solver SHALL choose tiles so all shared corners and edges match, producing seamless
  transitions

#### Scenario: Animated tiles
- **WHEN** a tile declares animation frames
- **THEN** its chunk SHALL be re-submitted with an updated frame index without rebuilding geometry

### Requirement: 2D lighting
The engine SHALL support 2D lighting with: point and directional 2D lights (with texture cookies,
energy, range, and falloff), normal-map and specular-map response for 2D sprites, per-light
layer masks, and blend modes (add, subtract, multiply).

2D lighting SHALL be optional per layer, so a UI layer is unlit while a gameplay layer is lit.

#### Scenario: Normal-mapped sprite
- **WHEN** a sprite has a normal map and a 2D light is nearby
- **THEN** it SHALL be shaded per pixel using the light's position and the sprite's normals

#### Scenario: Unlit UI layer
- **WHEN** a layer is marked unlit
- **THEN** 2D lights SHALL not affect it and no lighting cost SHALL be incurred for it

### Requirement: 2D shadows
2D shadow casters SHALL be polygon or polyline occluders with a culling mode (both sides,
clockwise only, counter-clockwise only).

Shadows SHALL be rendered per light into a shadow buffer, with filtering options (none, PCF) and
a softness parameter.

#### Scenario: One-sided occluder
- **WHEN** an occluder uses one-sided culling
- **THEN** light SHALL pass from one side and be blocked from the other, letting a light inside a
  room escape through the wall's inner face

### Requirement: Screen-space signed distance field
The engine SHALL rasterise 2D occluders into a screen-space signed distance field, sized by a
configurable oversize and scale factor.

The SDF SHALL be sampleable from 2D materials, and SHALL be exposed to the VFX system as a data
interface so 2D effects can collide against occluder geometry (see `vfx-system`).

#### Scenario: Particle collides with 2D geometry
- **WHEN** a 2D VFX effect samples the screen-space SDF data interface for collision
- **THEN** its particles SHALL collide with occluder geometry sampled from the SDF

#### Scenario: Oversize prevents edge popping
- **WHEN** the SDF is oversized beyond the viewport
- **THEN** occluders just off screen SHALL still contribute

### Requirement: Cameras and coordinate spaces
2D rendering SHALL support a `Camera2D` with position, zoom, rotation, offset, limits, and
smoothing, plus **pixel-perfect** modes that snap to integer pixel boundaries.

The engine SHALL define resolution-independence strategies: fixed viewport with letterboxing,
scaled canvas, and expanded canvas, each with documented behaviour on aspect-ratio change.

#### Scenario: Pixel-perfect rendering
- **WHEN** pixel-perfect mode is enabled with a nearest filter
- **THEN** sprite positions SHALL be snapped so texels map 1:1 to pixels without shimmer

#### Scenario: Aspect ratio change
- **WHEN** the window aspect changes with the "expand" strategy
- **THEN** more of the world SHALL become visible rather than the image stretching

### Requirement: Canvas groups and effects
The engine SHALL support compositing a group of 2D elements into an offscreen target so effects
(blur, colour adjustment, custom material) apply to the group as a whole rather than
per-element, avoiding double blending on overlaps.

#### Scenario: Group transparency
- **WHEN** overlapping sprites are placed in a canvas group with 50 % opacity
- **THEN** the group SHALL be composited once at 50 %, without the overlap appearing darker

### Requirement: 2D in world space
2D content SHALL be renderable into a 3D scene as a world-space canvas with a transform,
participating in depth testing and optionally in 3D lighting.

#### Scenario: In-world screen
- **WHEN** a 2D UI is placed on a monitor in a 3D scene
- **THEN** it SHALL be rendered into a texture and drawn on the monitor's surface with correct
  depth and lighting
