## MODIFIED Requirements

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
