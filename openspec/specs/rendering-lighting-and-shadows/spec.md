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
The **conventional** shadow path — used by profiles that do not enable virtual shadows, and as the
fallback where capabilities are absent — SHALL allocate point and spot light shadows from a **shadow
atlas** partitioned into tiles of several sizes.

This path SHALL remain fully supported. It is the correct answer for constrained hardware and the
required fallback; it is not deprecated by `virtual-shadows`.

Tile size SHALL be selected per light per frame from its projected screen coverage, with
hysteresis so a light does not oscillate between sizes.

Allocation SHALL prefer a free tile, then the least recently used tile whose owner has not been
allocated within a minimum retention period, so competing lights do not thrash.

Shadow maps SHALL be **cached across frames** for lights and casters that have not changed,
tracked by a version per light and per caster set.

Where a light's shadow mode is `Virtual`, its shadowing SHALL be governed by `virtual-shadows`
instead, and it SHALL consume no atlas tile.

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

#### Scenario: Modes coexist
- **WHEN** a scene mixes conventional and virtual shadowed lights
- **THEN** each SHALL use its declared path, and the atlas SHALL be sized only for the conventional
  ones

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
Under the **clustered** path, the number of lights affecting a single cluster SHALL be bounded, and
the number of shadowed lights per view SHALL be bounded by the shadow atlas budget.

Lights exceeding budgets SHALL be dropped deterministically by importance (screen coverage ×
intensity), and the shortfall SHALL be reported.

Under the **stochastic many-light** path, the per-cluster bound SHALL NOT apply: lights are
importance-sampled rather than iterated, and the bound becomes the sample count per pixel and the
visibility ray budget.

The active path SHALL be reported alongside light statistics, since a light limit that does not
apply is more confusing than one that does.

#### Scenario: Deterministic dropping
- **WHEN** the same scene is rendered twice with too many lights under the clustered path
- **THEN** the same lights SHALL be dropped both times, so the result does not flicker between
  frames

#### Scenario: The limit that applies is the one reported
- **WHEN** the stochastic path is active
- **THEN** statistics SHALL report sample counts and ray budgets rather than a per-cluster light
  limit that is not in force

### Requirement: Light channels
Lights SHALL declare a **channel mask**, and renderables SHALL declare the channels they receive,
so a light can be restricted to a subset of the world — characters only, a specific interior,
VFX only — for artistic control.

Channel resolution SHALL be a compact bitfield test during light assignment, not a per-object
light loop, so channels cost nothing at shading time.

Channels SHALL be a filter on assignment, and SHALL NOT create separate lighting passes.

#### Scenario: Character-only key light
- **WHEN** a light is restricted to the character channel
- **THEN** it SHALL illuminate characters and SHALL not be assigned to world geometry

#### Scenario: Channels do not cost per pixel
- **WHEN** channels are in use
- **THEN** the cost SHALL be in cluster assignment, not in a per-pixel loop over lights

### Requirement: Stochastic many-light direct lighting
The engine SHALL define a **stochastic direct lighting** path for scenes with very many shadowed
lights, in which each pixel samples a small number of lights chosen by importance rather than
evaluating every light in its cluster.

The path SHALL be: candidate generation from the GPU light set, **reservoir sampling** with
temporal and spatial reuse, ray-traced or shadow-map visibility for the selected samples, and
denoising through `denoising`.

Clustered lighting SHALL remain the **default shipping path**. The stochastic path SHALL be a
profile and scene decision, and content SHALL render correctly under either.

This path SHALL NOT be enabled without denoising, since its output is a noisy estimate; that
dependency SHALL be enforced by configuration validation rather than discovered visually.

Direct and indirect illumination SHALL remain separate solvers. Many-light direct lighting SHALL
NOT be implemented inside the GI system, so the two do not develop divergent lighting models.

#### Scenario: Thousands of shadowed lights
- **WHEN** a scene contains many thousands of shadowed area lights
- **THEN** the stochastic path SHALL evaluate a bounded number of samples per pixel rather than
  iterating the cluster's full light list

#### Scenario: Denoiser is required
- **WHEN** the stochastic path is enabled without denoising
- **THEN** configuration validation SHALL reject it with a diagnostic

#### Scenario: Same content, either path
- **WHEN** a scene is rendered with clustered lighting and with the stochastic path
- **THEN** both SHALL produce correct lighting, differing in cost, noise characteristics, and the
  number of shadowed lights that remain affordable

### Requirement: Area lights are lights the frame carries
The frame SHALL carry rect, disc, sphere and tube lights as lights of their own kinds, each with a
size in metres, a luminance in nits and, for a rect or a disc, an optional two-sided flag. The
forward pass SHALL shade an area light's diffuse term as the form factor of its emitting surface
and, when the frame names a linearly-transformed-cosine table, its specular term as that table's
integral over the surface; without a table it SHALL shade the diffuse term only. The device's
shading SHALL agree with the engine's host evaluation of the same light at the same surface. An
area light SHALL reach CyberGI as emitters radiating its flux. A frame with no area light and no
table SHALL be the frame as it was before area lights existed.

#### Scenario: No area light is the frame before
- **WHEN** a frame is drawn with no area light and no table
- **THEN** it SHALL be byte-identical to the frame drawn by the frame shader as it was before area
  lights existed

#### Scenario: The device shades as the host evaluates
- **WHEN** a floor is lit by a rect, a disc, a sphere and a tube light with the table named
- **THEN** every lit floor pixel SHALL match the host's area shading of the same light, diffuse and
  specular, within the stated tolerance

#### Scenario: Energy against a reference
- **WHEN** an area light's diffuse and specular terms are compared with a Monte-Carlo integration
  over its surface
- **THEN** the diffuse term SHALL agree within one per cent for a rect and the stated tolerance for
  the other shapes, and the specular term within the stated mean and worst-case bounds

### Requirement: Light mobility reaches the frame
A light SHALL carry a `Static`, `Stationary` or `Movable` mobility, `Movable` by default. A static
light SHALL be in no cluster list and no stochastic candidate set and SHALL change no pixel of the
frame; its index SHALL remain, so the lightmap's channels and the stochastic path's history keep
their identities. A stationary or movable light SHALL be shaded directly.

#### Scenario: A static light costs the frame nothing
- **WHEN** a frame is drawn with a light marked `Static`
- **THEN** no texel SHALL differ from the frame without that light and no cluster list SHALL name it
- **AND** the same light marked `Stationary` or `Movable` SHALL light the frame

### Requirement: Cluster lists are assigned on the device
When the device light assignment is enabled, the frame's cluster lists SHALL be built by a compute
pass from the frame's light records: each cluster's list SHALL be the lights of the host's list for
that cluster whose bounds reach the cluster's tile frustum, nearest first, bounded per cluster, with
the overflow counted. A thousand dynamic lights at 1920×1080 SHALL be assigned and shaded inside a
stated budget on the reference device.

#### Scenario: The device's lists are the host's, less the lights that miss the tile
- **WHEN** a frame of hundreds of lights is assigned on the device and on the host
- **THEN** no device list SHALL name a light the host's does not, every light the host's names that
  reaches the tile SHALL be in the device's, and the two frames SHALL be byte-identical

#### Scenario: A thousand lights inside the budget
- **WHEN** a thousand lights light a 1920×1080 frame
- **THEN** the device assignment SHALL take under 0.5 ms and the depth prepass's end to the opaque
  pass's end under 1.5 ms on the reference device, with no list overflowing

### Requirement: The stochastic many-light path is a device frame
The stochastic path SHALL draw, per pixel, light candidates in proportion to their power, resample
them into a reservoir, reuse reservoirs temporally with a capped history and spatially with
weights that keep the estimate unbiased, trace one visibility ray for the selected sample, and
denoise the diffuse result with the engine's denoiser. Its estimate SHALL be unbiased against a sum
over every light, and with reuse its bias SHALL stay inside a stated bound; its denoised signal's
noise and frame-to-frame change SHALL stay inside stated bounds. The choice between the clustered
and the stochastic path SHALL be made each frame from the cluster bound's drops and the frame
arbiter's shading allocation, leaving at once and returning only after a dwell.

#### Scenario: Unbiased against every light
- **WHEN** the stochastic estimate without reuse is averaged over many frames
- **THEN** its mean over the open floor SHALL be within one per cent of the exact sum over every
  light

#### Scenario: The denoiser is the engine's
- **WHEN** the path's denoised output is compared with `Denoiser::denoise` over the device's own
  noisy signal and reprojection
- **THEN** the two SHALL agree within the stated tolerance

#### Scenario: Either path, the same content
- **WHEN** a scene with no shadowing is drawn clustered and stochastically
- **THEN** the mean difference SHALL be within five per cent and the mean absolute per-pixel
  difference within twenty per cent of the clustered picture

#### Scenario: The arbiter's choice
- **WHEN** a frame's clusters drop lights at their bound
- **THEN** the selection SHALL move the frame to the stochastic path that frame
- **AND** once the clustered lighting fits the arbiter's allocation again, it SHALL return only
  after the dwell

### Requirement: Lights a game spawns for a moment
The engine SHALL provide a fixed pool of point, spot and area lights a game spawns with a lifetime
and a fade, addressed by generational handles, with no allocation per spawn; the live lights SHALL
be appended to the frame's lights each frame, faded, and a light whose lifetime ended SHALL be
freed. A directional light, a negative lifetime and a spawn into a full pool SHALL be refused, and
the refusal counted.

#### Scenario: A flash fades and is freed
- **WHEN** a light with a lifetime of 0.1 s and a quadratic fade is spawned and advanced 0.05 s
- **THEN** it SHALL contribute a quarter of its intensity, and after its lifetime SHALL be gone
  from the frame and its handle SHALL no longer answer
