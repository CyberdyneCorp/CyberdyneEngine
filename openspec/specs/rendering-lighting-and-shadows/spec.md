# rendering-lighting-and-shadows Specification

## Purpose

Defines light types and their parameters, physical light units, shadow map allocation and
filtering, cascaded shadows for directional lights, point and spot shadow projections, and
decals and light functions.

## Requirements

### Requirement: Light types
The engine SHALL support: `Directional`, `Point`, `Spot`, `Rect` (area), `Disc` (area), and
`Tube` (area) lights.

Common parameters: colour, intensity, temperature (Kelvin, converted to colour), range, an
attenuation model, a shadow toggle with bias parameters, a cull mask, a `Static` / `Stationary` /
`Movable` mobility classification, and separate diffuse and specular scale factors.

Spot lights SHALL additionally have inner and outer cone angles; area lights SHALL have
dimensions and an optional two-sided flag.

#### Scenario: Mobility drives the lighting path
- **WHEN** a light is marked `Static`
- **THEN** its contribution MAY be fully baked into lightmaps and it SHALL contribute no runtime
  cost; `Stationary` lights SHALL have baked indirect but dynamic direct light and shadows;
  `Movable` lights SHALL be fully dynamic

#### Scenario: Colour temperature
- **WHEN** a light specifies 3200 K
- **THEN** the engine SHALL convert it to a linear RGB tint using a black-body approximation,
  multiplied by the light's colour

### Requirement: Physical light units
Light intensity SHALL be expressible in **physical units**: lux for directional lights, lumens
for point and spot lights, and nits (cd/m²) for area lights.

Physical intensity SHALL be combined with the camera's exposure (aperture, shutter speed, ISO)
so a physically-configured scene is correctly exposed without arbitrary multipliers.

A non-physical **arbitrary units** mode SHALL remain available for stylised projects.

#### Scenario: Physically consistent scene
- **WHEN** a scene uses 100 000 lux sunlight and an EV-based camera
- **THEN** the exposed result SHALL match real-world expectations without per-light tuning

#### Scenario: Switching modes
- **WHEN** a project switches from arbitrary to physical units
- **THEN** the engine SHALL provide a documented conversion and flag lights whose values are
  implausible

### Requirement: Area light evaluation
Area lights SHALL be evaluated with **linearly transformed cosines** (LTC), using precomputed
lookup tables for the GGX BRDF, giving correct soft shadowing of the highlight and correct
diffuse falloff without stochastic sampling.

Textured area lights SHALL sample a filtered representation of the emitter, selected by
roughness.

#### Scenario: Rect light highlight
- **WHEN** a rectangular light illuminates a glossy surface
- **THEN** the specular highlight SHALL take the light's shape, elongating with roughness

#### Scenario: LTC fallback
- **WHEN** the shading model does not support LTC (hair, cloth)
- **THEN** the area light SHALL be approximated by a representative point with a documented
  approximation

### Requirement: Shadow atlas allocation
Point and spot light shadows SHALL share a **shadow atlas** partitioned into tiles of several
sizes.

Tile size SHALL be selected per light per frame from its projected screen coverage, with
hysteresis so a light does not oscillate between sizes.

Allocation SHALL prefer a free tile, then the least recently used tile whose owner has not been
allocated within a minimum retention period, so competing lights do not thrash.

Shadow maps SHALL be **cached across frames** for lights and casters that have not changed,
tracked by a version per light and per caster set.

#### Scenario: Nearby light gets a larger tile
- **WHEN** a light covers a large fraction of the screen
- **THEN** it SHALL receive a larger atlas tile, subject to availability

#### Scenario: Static shadow is reused
- **WHEN** a static light with only static casters has already been rendered
- **THEN** its shadow map SHALL be reused without re-rendering

#### Scenario: Atlas is oversubscribed
- **WHEN** more lights request shadows than the atlas can hold
- **THEN** lights SHALL be prioritised by screen coverage and importance, and those that miss out
  SHALL render unshadowed with the shortfall reported

### Requirement: Directional shadow cascades
Directional lights SHALL use **cascaded shadow maps** with a configurable cascade count
(default 4) and split distances derived from a blend of logarithmic and uniform distributions,
with an artist-controllable blend factor.

Cascades SHALL be **stabilised**: the shadow projection SHALL be snapped to texel increments and
sized by a bounding sphere of the cascade frustum, so shadows do not swim as the camera rotates.

Cascade transitions SHALL be blended over a configurable band.

Depth range SHALL be maximised by fitting the near plane to the actual caster bounds
(equivalently, flattening geometry in front of the cascade near plane).

#### Scenario: No shadow swimming
- **WHEN** the camera rotates
- **THEN** texel snapping SHALL keep shadow edges stable rather than crawling

#### Scenario: Cascade blending
- **WHEN** a fragment lies in a cascade transition band
- **THEN** both cascades SHALL be sampled and blended, avoiding a visible seam

#### Scenario: Distant shadow fade
- **WHEN** a fragment is beyond the last cascade
- **THEN** shadowing SHALL fade out over a configurable distance rather than ending abruptly

### Requirement: Point and spot shadow projection
Point light shadows SHALL be rendered as **cube shadow maps** (six faces), with per-face frustum
culling so faces with no casters are skipped.

Spot light shadows SHALL use a single perspective projection matching the cone.

#### Scenario: Empty cube face is skipped
- **WHEN** no casters fall within one face of a point light's cube
- **THEN** that face SHALL not be rendered

### Requirement: Shadow filtering
Shadow lookups SHALL use hardware comparison sampling with reversed-Z comparison, and SHALL
support:

- **PCF** with a rotated Poisson disk kernel, the rotation derived per pixel from interleaved
  gradient noise seeded with the frame index so temporal accumulation resolves the noise
- **PCSS** — a blocker search estimating average occluder depth, from which a penumbra radius
  scales the filter kernel, giving contact-hardening soft shadows
- **Sample counts** as specialization constants driven by the quality setting

Bias SHALL combine a constant depth bias, a slope-scaled bias, and a **normal offset** that
displaces the lookup position along the surface normal proportionally to shadow texel size.

#### Scenario: Contact-hardening shadow
- **WHEN** PCSS is enabled and an object touches the floor
- **THEN** the shadow SHALL be sharp at contact and soften with distance

#### Scenario: Peter-panning is avoided
- **WHEN** normal offset bias is used instead of a large constant bias
- **THEN** shadows SHALL remain attached at contact points

#### Scenario: Temporal noise resolves
- **WHEN** TAA is enabled
- **THEN** the per-frame kernel rotation SHALL convert banding into noise that TAA averages away

### Requirement: Shadow rendering optimisation
Shadow passes SHALL: use a depth-only pipeline with no fragment shader except for alpha-tested
materials; use coarser LODs than the main view; support a per-light caster distance limit; and
support **shadow proxy meshes** — simplified geometry used only for shadow casting.

#### Scenario: Alpha-tested casters
- **WHEN** foliage casts shadows
- **THEN** only alpha-tested materials SHALL bind a fragment shader in the shadow pass

#### Scenario: Shadow proxy
- **WHEN** a mesh declares a shadow proxy
- **THEN** the proxy SHALL be rendered into shadow maps and the full mesh into the main view

### Requirement: Light functions and cookies
Point and spot lights SHALL support a **cookie** texture masking their emission, and directional
lights SHALL support a cookie projected in light space, for effects such as window patterns and
cloud shadows.

#### Scenario: Animated cookie
- **WHEN** a cloud-shadow cookie scrolls
- **THEN** the directional light's contribution SHALL be modulated by it without additional
  shadow map cost

### Requirement: Decals
Decals SHALL be projected oriented boxes writing into albedo, normal, roughness, metallic, and
emission before lighting, blended by per-channel weights.

Decals SHALL be **GPU scene residents**, not per-decal draw calls: a decal is an instance with a
transform, bounds, a material reference, and sort order, culled and gathered on the GPU like any
other instance, and applied through tile or cluster assignment.

The system SHALL be designed for very large decal counts — persistent damage, bullet impacts,
splatter — with a **decal budget** governed by the renderer budget arbiter and eviction of the
least important decals by age, screen coverage, and importance when the budget is reached.

Decals SHALL support: angle-based fade against the receiving surface normal, distance fade, a sort
order, a layer mask, and normal blending that preserves the receiver's detail.

Decals SHALL participate in cluster assignment as a distinct element type.

#### Scenario: Decal on a steep surface
- **WHEN** the receiving normal deviates beyond the decal's fade angle
- **THEN** the decal SHALL fade out rather than stretch

#### Scenario: Decal ordering
- **WHEN** decals overlap
- **THEN** they SHALL be applied in sort order, so a later decal can cover an earlier one

#### Scenario: Many decals, no draw call per decal
- **WHEN** tens of thousands of impact decals are present
- **THEN** they SHALL be culled and applied as GPU scene instances, without a CPU draw call each

#### Scenario: Budget evicts the least important
- **WHEN** the decal budget is reached and a new decal is spawned
- **THEN** the least important existing decal SHALL be evicted deterministically, and the eviction
  SHALL be reportable

### Requirement: Light culling and limits
The number of lights affecting a single cluster SHALL be bounded, and the number of shadowed
lights per view SHALL be bounded by the shadow atlas budget.

Lights exceeding budgets SHALL be dropped deterministically by importance (screen coverage ×
intensity), and the shortfall SHALL be reported.

#### Scenario: Deterministic dropping
- **WHEN** the same scene is rendered twice with too many lights
- **THEN** the same lights SHALL be dropped both times, so the result does not flicker between
  frames
