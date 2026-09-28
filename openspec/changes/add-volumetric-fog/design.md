# Design

## Context

`rendering-post-processing`'s froxel arithmetic has existed since M7: `froxel_slice_depth` and its
inverse, `henyey_greenstein`, and `integrate_froxel`, the analytic integral of a homogeneous slice.
`sky::AerialPerspectiveTable` (M10, applied by `add-aerial-perspective`) already stores what the air
does over that froxel distribution, and `cy/aerial_perspective.slang` samples it for a surface. What
did not exist is the medium, the lighting of it through a shadow map, a device pass that fills a
volume, and a surface seen through one.

## Decisions

### One march per froxel column, not four passes

The requirement names four stages — density injection, lighting injection, filtering, integration.
They are one loop here, one thread per (x, y) column, front to back through every slice and a fixed
number of sub-steps a slice. At each sub-step's midpoint the medium is sampled, the shadow map is
compared, the source term `J = sun_scattering E V + scattering L_ambient + emission` is formed and
one analytic step of `integrate_froxel` is taken; at each slice's far edge the column stores what it
has accumulated from the eye.

The alternative — a density volume, a lit in-scattering volume, a filter and an integration pass —
stores two intermediate volumes, and its lighting is evaluated once per froxel centre. A froxel far
from the camera is metres deep, and a shaft between two columns 1.9 m apart is narrower than that;
reading the shadow map at the sub-steps resolves it where a froxel-centre sample would not. The
fused loop costs no intermediate memory and has no barrier between stages.

**Consequence: no filter and no history.** Sub-step midpoints are fixed, so the volume is
deterministic: there is no noise for a filter or a temporal reprojection to remove, and the volume is
rebuilt whole every frame. What that costs is aliasing of shafts thinner than a froxel. The
requirement's temporal reprojection through `temporal-rendering` is recorded as not built.

### The camera is the aerial perspective table's

The fog uses `sky::AerialPerspectiveTable::View`'s basis and tangents, the column ray with +y up, and
`froxel_slice_depth` along forward, integrated from the eye — so the texture's header is that
table's five header words plus the eye, and one sampler shape reads both. A second froxel geometry
would put the fog and the air on two depth axes, and the seam between them would move with the
camera.

### The light is the frame's, and the quantities are physical

The medium is lit by the same sun illuminance and the same ambient radiance the frame's surfaces are
shaded with, in the frame's own units; an isotropic ambient is scattered by `sigma_s` because a
phase function integrates to one. The medium is stated as an extinction (through Koschmieder's
meteorological visibility, `3.912 / V`), an exponential scale height above a base altitude, an
albedo and a Henyey-Greenstein `g`. There is no fog colour and no fog distance. The FREE PARAMETERS
are therefore the air itself — the visibility, the layer's scale height and base, the droplets'
albedo and `g` — each stated as content (`content/beauty/shot.cyshot`, `samples/10-world/
frame.cypost`) with the physical reason for its value; the volume's resolution and sub-steps are the
only renderer choices.

### Two targets from one source

`FogTarget::Texture` writes an `Rgba32Sfloat` texture the forward pass reads through a texture-table
slot: every other extra input the frame reads (shadow map, contact term, ambient occlusion, probes)
arrives that way, so the frame's set layout does not change. `FogTarget::AerialTable` compiles the
same march with `CY_FOG_AIR_TABLE` and writes `sky::pack_aerial_perspective`'s layout into a buffer
with the atmosphere composited in, which `samples/10-world` binds where its air table was — its
world shader does not change.

### Fog and air as one medium per slice

With the atmosphere's table bound, each slice's stretch of air is read out of it at the stretch's two
ends along the column's ray: `T = T(far) / T(near)`, `S = (S(far) - S(near)) / T(near)`. That is
turned into the per-channel extinction `-ln(T) / length` and the source `J` of a homogeneous medium
that transmits and adds exactly `T` and `S` over the stretch, and the air is marched WITH the fog
at every sub-step. The exact-for-homogeneous-slices form means an empty fog reproduces the table and
a switched-off table leaves the fog bit for bit as it was; applying the two one after the other would
put all of one medium in front of all of the other.

### Off is absent; empty is the identity

With the setting off nothing is declared and no slot or binding changes, so every consumer's
fragment is the arithmetic it was. An empty medium writes transmittance exactly one and in-scattering
exactly zero, and the lookup's interpolation is `a + (b - a) t` — the intrinsic `lerp` may be
evaluated as `a (1 - t) + b t`, which does not return exactly one between two texels holding one.

## Where it sits in the frame

`FramePassKind::VolumetricFog`, after the shadow pass and the screen-space passes and before the
opaque pass: step 3 of the post chain, "volumetric fog composite", which a forward renderer does per
surface. It needs no prepass. `samples/10-world` declares the table variant's march itself, before
its water pictures, because those are drawn with the lit path before the frame's stages and read the
same table.

## Risks

- Shafts thinner than a froxel alias, and a camera move shows it; a history would hide it and does
  not exist.
- The volume is rebuilt every frame: 160 x 90 columns x 96 slices x 4 sub-steps in the beauty shot.
- The shaded sea in `samples/10-world` (`shaders/water.slang`) reads no table and is not fogged,
  exactly as it is not aerial-perspective'd; the dome is sky and is not fogged either.
