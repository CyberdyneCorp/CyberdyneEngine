# rendering-global-illumination Specification

## Purpose

Defines indirect lighting: baked lightmaps and light probes, dynamic diffuse GI, reflection
probes and screen-space reflections, and the sky model that feeds ambient lighting.

The strategy is layered rather than betting on one technique: bake what is static, use probe-based
dynamic GI for what moves, and use screen space to add detail where the information is available.

## Requirements

### Requirement: GI strategy layers
The engine SHALL support these indirect lighting sources, combinable per scene:

| Layer | Covers | Cost |
|---|---|---|
| Lightmaps | Static geometry lit by static lights | Bake time; near-zero runtime |
| Irradiance volumes | Dynamic objects in baked environments | Small runtime |
| Dynamic diffuse GI | Fully dynamic scenes and lighting | Moderate runtime |
| Reflection probes | Localised specular | Small runtime; capture cost |
| Screen-space reflections | Contact-accurate specular detail | Moderate runtime |

The renderer SHALL combine them without double counting: a surface with a lightmap SHALL NOT
also accumulate dynamic diffuse GI for the same bounce.

#### Scenario: No double counting
- **WHEN** a lightmapped surface is inside a dynamic GI volume
- **THEN** it SHALL take indirect diffuse from the lightmap only, and the dynamic contribution
  SHALL be excluded for it

#### Scenario: Fallback chain for specular
- **WHEN** a screen-space reflection ray fails
- **THEN** the result SHALL fall back to the nearest reflection probe, then to the sky, blended
  by confidence rather than switching abruptly

### Requirement: Lightmap baking
The engine SHALL provide an offline **GPU path tracer** producing lightmaps for static geometry.

The bake SHALL: gather static meshes and their UV2 charts, build an acceleration structure,
path-trace direct and indirect lighting for a configurable bounce count, denoise the result,
dilate and pad chart borders, and pack into atlases.

Bake outputs SHALL be selectable: **irradiance only** (single texture), **directional** (an
irradiance plus a dominant direction, preserving normal-map response), or **spherical harmonics**
(L1, preserving more directionality at higher memory cost).

The bake SHALL support: per-object lightmap resolution scaling, a global texel density, emissive
surfaces as light sources, transparent and alpha-tested occlusion, and a **shadow mask** allowing
stationary lights to keep dynamic direct light with baked shadows.

Baking SHALL be **incremental** where possible: unchanged geometry and lighting SHALL reuse
previous results.

#### Scenario: Normal maps still respond
- **WHEN** directional lightmaps are baked
- **THEN** a normal-mapped surface SHALL still show normal-map detail in indirect lighting

#### Scenario: Stationary light with shadow mask
- **WHEN** a light is marked `Stationary` with shadow-mask baking
- **THEN** its indirect contribution SHALL be baked while its direct light is computed at runtime
  using the baked shadow term

#### Scenario: Incremental rebake
- **WHEN** one object is moved in a large baked level
- **THEN** the bake SHALL re-solve the affected region rather than the whole level

### Requirement: UV2 and chart packing
Static meshes participating in lightmapping SHALL have a **UV2** channel generated at import time
if not authored, using automatic unwrapping with a configurable texel density, chart padding, and
angle and area distortion limits.

Charts SHALL be packed into atlases with padding sufficient for bilinear filtering and mip
generation at the bake resolution.

#### Scenario: Unwrap is cached
- **WHEN** a mesh is reimported without geometry changes
- **THEN** the cached UV2 unwrap SHALL be reused

#### Scenario: Seam artifacts
- **WHEN** charts meet at a seam
- **THEN** border texels SHALL be dilated and seam colours reconciled so the seam is not visible

### Requirement: Irradiance volumes and light probes
The engine SHALL support **irradiance volumes**: 3D grids of baked probes storing spherical
harmonics irradiance, used to light dynamic objects inside baked environments.

Probes SHALL be placeable as a regular grid within a volume, adaptively subdivided near
geometry, or hand-placed.

Sampling SHALL be trilinear across the eight surrounding probes, weighted by a **visibility**
term so a probe on the other side of a wall does not leak light.

#### Scenario: Dynamic object lit by baked environment
- **WHEN** a character walks through a baked room
- **THEN** it SHALL be lit by interpolated probe irradiance, not by the lightmap

#### Scenario: Light leaking is suppressed
- **WHEN** a probe lies inside geometry or on the far side of a wall
- **THEN** its weight SHALL be reduced or zeroed by the visibility term

### Requirement: Dynamic diffuse global illumination
For scenes that cannot be baked, the engine SHALL provide **dynamic diffuse GI** based on a
cascaded grid of runtime-updated irradiance probes with per-probe visibility, updated
incrementally across frames.

Each cascade SHALL cover a progressively larger volume at lower probe density and SHALL **scroll**
with the camera in probe-sized increments, re-solving only newly exposed regions.

Probe update SHALL trace rays against a scene representation — a ray tracing acceleration
structure where the device supports it, or a signed distance field / voxel representation
otherwise — accumulate radiance into an octahedral irradiance and visibility encoding, and blend
temporally with a configurable hysteresis.

Multi-bounce SHALL be approximated by feeding the previous frame's probe irradiance back into the
gather.

#### Scenario: Camera moves
- **WHEN** the camera translates by one probe spacing
- **THEN** the cascade SHALL scroll and only the newly entered slab SHALL be re-solved

#### Scenario: Lighting change converges
- **WHEN** a light is switched on
- **THEN** probe irradiance SHALL converge over a configurable number of frames, with the
  convergence rate exposed as a quality setting

#### Scenario: Ray tracing unavailable
- **WHEN** the device lacks ray tracing
- **THEN** the SDF/voxel tracing path SHALL be used with the same probe representation and
  sampling code

### Requirement: Reflection probes
Reflection probes SHALL capture the surrounding scene into a roughness-filtered octahedral
radiance map, with:

- capture modes: **baked** (once, offline), **on demand**, and **realtime** (amortised across
  frames)
- **box** and **sphere** influence volumes with a blend distance
- **box projection** (parallax correction) so reflections align with room geometry
- an importance value resolving overlapping probes
- an interior flag excluding the sky

Probes SHALL be assigned to fragments through cluster assignment and blended by influence weight.

#### Scenario: Parallax-corrected reflection
- **WHEN** box projection is enabled
- **THEN** the reflection vector SHALL be intersected with the probe's box and re-aimed at the
  hit point, so a wall reflects at the right place

#### Scenario: Realtime probe is amortised
- **WHEN** a realtime probe updates
- **THEN** its faces and filtering mips SHALL be spread across several frames under a budget

### Requirement: Screen-space reflections
SSR SHALL march the depth buffer along the reflection vector using a hierarchical depth pyramid,
then resolve with roughness-dependent filtering and temporal accumulation.

SSR SHALL produce a **confidence** value per pixel (ray hit, screen edge proximity, thickness
mismatch), used to blend toward reflection probes rather than switching.

SSR SHALL be a per-view toggle with quality settings for ray count, step count, and resolution.

#### Scenario: Ray leaves the screen
- **WHEN** a reflection ray exits the viewport
- **THEN** confidence SHALL fall toward zero near the edge and the probe fallback SHALL take over
  smoothly

#### Scenario: Rough surface
- **WHEN** the surface is rough
- **THEN** the resolve SHALL widen its filter and reduce ray count, since the lobe is broad

### Requirement: Ray-traced effects
Where the device supports ray tracing, the engine SHALL optionally use it for: reflections
(replacing or complementing SSR beyond the screen), soft shadows, ambient occlusion, and dynamic
GI probe tracing.

Ray-traced features SHALL be capability-gated and each SHALL have a non-ray-traced fallback, so
no content depends on their presence.

#### Scenario: Ray-traced reflection beyond the screen
- **WHEN** ray tracing is available and SSR confidence is low
- **THEN** a traced ray SHALL supply the reflection instead of falling back to a probe

#### Scenario: Acceleration structure maintenance
- **WHEN** dynamic geometry moves
- **THEN** its bottom-level structures SHALL be refit and the top-level structure rebuilt per
  frame, with a budget for full rebuilds

### Requirement: Sky and atmosphere
The engine SHALL provide sky models: a **physical atmosphere** (Rayleigh and Mie scattering with
precomputed transmittance and multiple-scattering tables), a **cubemap or HDRI** sky, a
**gradient** sky, and a fully **custom shader** sky.

The sky SHALL produce: the background, a filtered radiance map for specular IBL, and irradiance
for ambient diffuse — updated when the sky changes, incrementally where it changes continuously
(a moving sun).

Volumetric clouds SHALL be a planned addition whose interface (a participating-media layer
integrated with atmosphere and aerial perspective) is reserved but not specified here.

#### Scenario: Time of day
- **WHEN** the sun rotates continuously
- **THEN** atmosphere tables SHALL update incrementally and the radiance and irradiance SHALL
  follow without a visible step

#### Scenario: Aerial perspective
- **WHEN** the physical atmosphere is enabled
- **THEN** distant geometry SHALL receive scattering consistent with the sky, not a separately
  tuned fog

### Requirement: GI debug visualisation
The engine SHALL provide debug views: lightmap UV density and charts, lightmap texel resolution,
probe positions and their irradiance, probe visibility, dynamic GI cascade bounds, reflection
probe influence volumes and their captures, SSR confidence, and the combined indirect diffuse and
specular buffers in isolation.

#### Scenario: Diagnosing a light leak
- **WHEN** light leaks through a wall
- **THEN** the probe visibility debug view SHALL show which probes contribute incorrectly
