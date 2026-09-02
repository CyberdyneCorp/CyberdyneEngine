# rendering-post-processing Specification

## Purpose

Defines the post-process chain: ambient occlusion, subsurface scattering, volumetric fog,
exposure, depth of field, motion blur, bloom, tonemapping and colour grading, anti-aliasing, and
temporal upscaling — including their order, their dependencies, and the colour space each
operates in.

## Requirements

### Requirement: Chain order and colour space
The post-process chain SHALL execute in this order, operating on **linear HDR** scene colour
until tonemapping:

1. Ambient occlusion (applied to indirect diffuse during shading, computed before it)
2. Subsurface scattering resolve
3. Volumetric fog composite
4. Screen-space reflections composite
5. Temporal anti-aliasing **or** temporal upscaling
6. Auto-exposure luminance measurement
7. Depth of field
8. Motion blur
9. Bloom
10. Exposure application
11. Tonemapping → display-referred colour
12. Colour grading (LUT)
13. Display-space effects: vignette, chromatic aberration, film grain, dithering
14. Post-tonemap anti-aliasing (FXAA/SMAA) if selected
15. Output encoding (sRGB or HDR transfer function)

Effects operating after tonemapping SHALL be documented as display-referred; effects before it as
scene-referred.

#### Scenario: Bloom is scene-referred
- **WHEN** bloom runs
- **THEN** it SHALL operate on pre-exposure linear HDR colour, so its threshold is meaningful in
  physical units

#### Scenario: Film grain is display-referred
- **WHEN** film grain is applied
- **THEN** it SHALL be applied after tonemapping so its strength is perceptually uniform

### Requirement: Ambient occlusion
The engine SHALL implement **ground-truth ambient occlusion (GTAO)** — horizon-based visibility
integration with a cosine-weighted term — computed from depth and normals at half or full
resolution.

Where ray tracing is available, ray-traced ambient occlusion SHALL be selectable, tracing through
`ray-tracing-infrastructure` as one more consumer of the illumination tiers.

Filtering and temporal accumulation SHALL be performed by the shared denoiser (see `denoising`)
with the signal declared as a **visibility term**, so contact detail is preserved rather than
filtered as colour.

AO SHALL produce both a visibility term and a **bent normal**, the latter used to improve indirect
specular occlusion.

AO SHALL modulate **indirect** lighting only; applying it to direct light SHALL be an explicitly
non-physical artistic option, off by default.

#### Scenario: AO does not darken direct light
- **WHEN** AO is enabled with default settings
- **THEN** a directly lit surface SHALL be unaffected, while its ambient contribution is occluded

#### Scenario: Bent normal improves specular
- **WHEN** bent normals are available
- **THEN** indirect specular SHALL use them for occlusion, avoiding reflections from occluded
  directions

#### Scenario: Contact detail survives denoising
- **WHEN** ray-traced AO is denoised
- **THEN** it SHALL be treated as occlusion, and contact darkening SHALL not be blurred away

### Requirement: Subsurface scattering
Subsurface scattering SHALL be resolved in screen space: diffuse lighting for SSS materials SHALL
be separated, blurred with a **separable burley diffusion profile** whose radius scales with the
material's scattering distance and per-pixel depth, and recombined with specular.

A **transmittance** term SHALL be evaluated during shading using a thickness estimate, so light
passes through thin geometry such as ears and leaves.

#### Scenario: Skin softness
- **WHEN** an SSS material with a skin profile is lit
- **THEN** the diffuse response SHALL show characteristic red bleeding at shadow boundaries

#### Scenario: Specular is not blurred
- **WHEN** SSS is resolved
- **THEN** the specular term SHALL be preserved sharp and recombined after the diffuse blur

### Requirement: Volumetric fog
Volumetric fog SHALL be computed in a **froxel volume** — a camera-frustum-aligned 3D texture with
exponential depth distribution — through: density injection (global fog plus fog volumes),
lighting injection (per-froxel light evaluation with shadow sampling), filtering, and front-to-back
scattering and transmittance integration.

Indirect lighting of the medium SHALL be taken from the **radiance cache** (see
`rendering-global-illumination`) rather than computed by a fog-specific indirect term, so fog in an
indirectly lit interior is lit consistently with the surfaces around it.

Fog volumes SHALL be authorable as boxes, spheres, cones, cylinders, and via a density function,
each with an emissive and albedo colour and an anisotropy (Henyey-Greenstein `g`) parameter.

**Temporal reprojection** SHALL use the framework in `temporal-rendering` rather than a
fog-specific history, so the low-resolution volume converges without visible noise.

Aerial perspective from the physical atmosphere SHALL be composited consistently with fog rather
than applied separately.

#### Scenario: Light shafts
- **WHEN** a shadowed spot light shines through fog
- **THEN** per-froxel shadow sampling SHALL produce visible shafts

#### Scenario: Fog volume with anisotropy
- **WHEN** `g` is positive and the view looks toward a light
- **THEN** forward scattering SHALL brighten the fog

#### Scenario: Fog matches the room it is in
- **WHEN** fog fills an interior lit only by bounce light
- **THEN** it SHALL take indirect radiance from the radiance cache and match the surrounding
  surfaces

### Requirement: Exposure
The engine SHALL support **manual exposure** (EV, or aperture / shutter / ISO) and
**auto-exposure** driven by a luminance histogram computed by compute shaders over the scene
colour.

Auto-exposure SHALL support: a metering mask, histogram percentile-based target selection to
reject outliers, minimum and maximum EV clamps, separate adaptation speeds for brightening and
darkening, and an exposure compensation curve keyed on measured luminance.

Exposure SHALL be applied as a scalar multiply before tonemapping and SHALL also be published to
shaders so emissive values can be expressed in physical units.

#### Scenario: Bright window does not blow out the room
- **WHEN** a small bright region is in view
- **THEN** percentile-based metering SHALL reject the outlier and expose for the room

#### Scenario: Adaptation speed
- **WHEN** the camera moves from dark to bright
- **THEN** exposure SHALL adapt at the configured rate, clamped to the EV range

### Requirement: Depth of field
Depth of field SHALL be computed from a physically parameterised circle of confusion derived from
focal length, aperture, focus distance, and sensor size, with an artistic override.

The implementation SHALL use a gather-based approach with separate near and far fields, a
configurable bokeh shape (circular, hexagonal, or a custom aperture texture), and correct
occlusion handling so near-field blur bleeds over in-focus geometry.

#### Scenario: Near-field bleeding
- **WHEN** an out-of-focus object is in front of a focused one
- **THEN** its blur SHALL extend over the focused object, not be clipped to its silhouette

#### Scenario: Autofocus
- **WHEN** autofocus targets an entity
- **THEN** focus distance SHALL track it with a configurable speed

### Requirement: Motion blur
Motion blur SHALL use the velocity buffer with a tile-based maximum-velocity approach:
compute per-tile maximum velocity, then gather along the dominant direction with depth-aware
weighting so background does not smear over foreground.

Blur amount SHALL be parameterised by shutter angle, with separate per-object and camera blur
scaling.

#### Scenario: Fast object over static background
- **WHEN** an object moves quickly across a static background
- **THEN** the object SHALL blur and the background SHALL stay sharp

#### Scenario: Blur respects shutter
- **WHEN** shutter angle is 180°
- **THEN** blur length SHALL correspond to half a frame of motion

### Requirement: Bloom
Bloom SHALL be produced by a progressive downsample and upsample chain with a **soft threshold**
knee, physically-motivated by default (energy scattered from bright sources) with an artistic
intensity control.

The implementation SHALL use a Karis average on the first downsample to suppress fireflies, and a
tent filter on upsample to avoid blocky artifacts.

An optional **lens dirt** and **anamorphic stretch** SHALL be supported.

#### Scenario: Firefly suppression
- **WHEN** a single very bright pixel appears
- **THEN** the Karis average SHALL prevent it from producing a flickering bloom star

#### Scenario: Energy-conserving default
- **WHEN** bloom is enabled with default settings
- **THEN** total image energy SHALL be approximately preserved, redistributed rather than added

### Requirement: Tonemapping
The engine SHALL provide these tonemap operators: `None` (clamp), `Reinhard`,
`ReinhardExtended`, `ACES` (the fitted RRT+ODT approximation), `AgX`, and `Custom` (a
user-supplied curve or LUT).

**AgX** SHALL be the default, for its hue stability and gentle highlight desaturation.

Tonemapping SHALL be parameterised for the output transfer function: sRGB for SDR, PQ or scRGB
for HDR displays, with the display's peak luminance taken into account.

#### Scenario: Highlights desaturate, not hue-shift
- **WHEN** a saturated bright light is tonemapped with AgX
- **THEN** it SHALL roll off toward white without shifting hue

#### Scenario: HDR output
- **WHEN** an HDR display is active
- **THEN** tonemapping SHALL map to the display's luminance range rather than to SDR white

### Requirement: Colour grading
Colour grading SHALL support: white balance (temperature and tint), per-channel lift/gamma/gain,
shadows/midtones/highlights colour wheels with range boundaries, saturation, contrast, hue shift,
channel mixer, and a **3D LUT** (`.cube`) applied in log space.

Grading parameters SHALL be bakeable into a single 3D LUT at build time so the runtime cost is one
texture lookup.

#### Scenario: Grade baked to a LUT
- **WHEN** grading parameters are static
- **THEN** they SHALL be baked into a 3D LUT, and the runtime SHALL apply only that lookup

#### Scenario: Log-space LUT
- **WHEN** a LUT is applied
- **THEN** the input SHALL be converted to a log encoding first, so the LUT has adequate
  precision in shadows

### Requirement: Anti-aliasing
The engine SHALL support:

- **MSAA** (2×/4×/8×) in the forward pipeline
- **TAA** — velocity-reprojected temporal accumulation with neighbourhood clamping, a variance
  or YCoCg clipping strategy, and separate handling for disoccluded regions
- **FXAA** and **SMAA** as post-tonemap spatial options
- **Alpha-to-coverage** for masked materials under MSAA

TAA SHALL consume the **temporal framework** (see `temporal-rendering`) for jitter, motion
vectors, history storage, reprojection, disocclusion classification, and history invalidation.
It SHALL NOT implement its own.

TAA SHALL provide a sharpening pass to counteract the softening it introduces.

#### Scenario: Ghosting is suppressed
- **WHEN** an object moves over a contrasting background
- **THEN** neighbourhood clamping SHALL reject stale history, avoiding a trail

#### Scenario: Disocclusion
- **WHEN** geometry is revealed from behind an occluder
- **THEN** the temporal framework SHALL classify those pixels as disoccluded, history SHALL be
  rejected for them, and they SHALL be reconstructed spatially

#### Scenario: TAA requires prerequisites
- **WHEN** TAA is requested without motion vectors
- **THEN** the temporal framework SHALL enable their production, or TAA SHALL be refused with a
  diagnostic

#### Scenario: Camera cut is handled by the framework
- **WHEN** the camera cuts
- **THEN** TAA's history SHALL be invalidated by the framework's cut event, not by logic TAA
  implements itself

### Requirement: Temporal upscaling
The engine SHALL support rendering the 3D scene at a reduced internal resolution and
reconstructing it at output resolution, with UI drawn at native resolution.

Upscalers SHALL be pluggable behind one interface taking colour, depth, motion vectors, exposure,
and jitter — the latter three supplied by the temporal framework. The engine SHALL ship a
**built-in temporal upscaler** and SHALL define the integration seams for vendor upscalers (FSR,
DLSS, XeSS, MetalFX) as optional modules, since their licensing and distribution differ. Public
renderer interfaces SHALL NOT depend on vendor types.

Dynamic resolution scaling SHALL be supported, and internal resolution SHALL be a **budget
allocation held by the renderer budget arbiter** (see `rendering-architecture`), not an
independent controller measuring frame time. Resolution SHALL adjust within configured bounds on
the arbiter's time constant.

#### Scenario: Half-resolution rendering
- **WHEN** internal resolution is 50 % with temporal upscaling
- **THEN** the 3D scene SHALL be reconstructed at full resolution using jitter, motion vectors,
  and history, while UI is composited at native resolution

#### Scenario: Dynamic resolution holds a budget
- **WHEN** GPU frame time exceeds the budget
- **THEN** the arbiter SHALL reduce the resolution allocation within configured bounds, and raise
  it again when headroom returns

#### Scenario: Vendor upscaler absent
- **WHEN** a vendor upscaler module is not present
- **THEN** the built-in upscaler SHALL be used with no content changes

#### Scenario: Resolution does not fight other controllers
- **WHEN** resolution is reduced, lowering every subsystem's measured cost
- **THEN** subsystem controllers SHALL NOT interpret the change as headroom to spend; only the
  arbiter SHALL re-evaluate allocations

### Requirement: Variable-rate shading
Where supported, the engine SHALL apply **variable-rate shading** driven by: a content-adaptive
mask derived from luminance variance and motion, and an externally supplied mask (XR foveation).

#### Scenario: Low-detail region shades coarsely
- **WHEN** a region has low luminance variance and high motion
- **THEN** it SHALL be shaded at a coarser rate, with the saving reported in statistics

### Requirement: Post-process configuration and volumes
Post-process settings SHALL be authored as a stack of **post-process volumes** with priorities,
blend distances, and an unbounded global volume, blended per-parameter by weight — so a camera
entering a volume smoothly transitions its grading, exposure, and fog.

#### Scenario: Entering an interior volume
- **WHEN** the camera crosses into a volume with different grading over a blend distance
- **THEN** parameters SHALL interpolate over that distance rather than switching

#### Scenario: Priority resolves overlap
- **WHEN** two volumes overlap
- **THEN** the higher priority SHALL dominate, with weights normalised across contributors

### Requirement: Performance and quality scaling
Every effect SHALL expose quality levels (off, low, medium, high, ultra) that map to concrete
parameters (resolution scale, sample counts, iteration counts), and SHALL declare its typical GPU
cost so a quality preset can be assembled against a frame budget.

#### Scenario: Quality preset
- **WHEN** a project selects the "medium" preset
- **THEN** each effect SHALL apply its medium parameters, and the sum of declared costs SHALL be
  reportable against the target frame time
