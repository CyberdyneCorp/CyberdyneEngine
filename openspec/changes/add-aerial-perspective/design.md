# Design

## Context

Every piece of `Aerial perspective` except the one that matters was built:

| piece | where | state before this change |
|---|---|---|
| the model | `sky::aerial_perspective()` (`atmosphere.h`) | single scattering and transmittance between two points, from the same `Atmosphere` as `sky_radiance()`; tested by `unit.render_sky` |
| the table | `sky::AerialPerspectiveTable` (`tables.h`) | a `rendering::FroxelVolume` of transmittance and in-scattering, integrated with the TABULATED transmittance and multiple scattering the sky uses; tested by `integration.render_sky_tables` against the model and against the sky at its far plane |
| the override | `sky::StylisedDistance` / `stylise()` | a declared blend toward a non-physical falloff |
| a consumer | — | none. No shader, no frame, no sample reads either. |

`samples/10-world` — the one frame in the tree that draws terrain kilometres away — shaded every
surface with a Lambert term and nothing between it and the eye, and drew its dome's clear sky with
`world_visual.slang`'s `composeClouds` gradient: two tuned colours blended by the sun's height. Even
a correct haze would have faded the terrain toward a colour that sky does not contain.

## Goals / Non-Goals

**Goals:**

- Every surface the world frame lights is attenuated by the air between it and the eye and receives
  the light scattered into that path, from the atmosphere the sky is lit by.
- The dome's clear sky is that same atmosphere, from the same tables, in the same radiance units, so
  a distant surface fades into the sky beside it rather than into a fog colour.
- A near surface is left as it was lit; the frame with aerial perspective off is bit-identical to
  the frame before.
- Each of those is a test that has been seen failing.

**Non-Goals:**

- The engine forward frame (`cy/frame.slang`). It draws no world today, and its shading and ambient
  terms are being edited by two parallel changes (soft shadows and probes). Its set-0 layout would
  need a new binding in `frame_pipelines.h`, a write in `frame_bindings.cpp` and regenerated
  `frame_spirv.h` / `frame_msl.h`. `cy/aerial_perspective.slang` is written as a standard-library
  module so that frame can import it; wiring it is recorded as what remains.
- The shaded sea. `shaders/water.slang` (from `add-water-shading`, merged while this change was in
  review) draws the sea by default and reads no table, so with water shading on the sea's own
  surface is not attenuated by the air in front of it; its refraction and reflection pictures are
  drawn with world.slang and do carry the air to the bed and the mirrored terrain. Wiring it means the water's set
  0 declaring the third binding, applying `cyApplyAerialPerspective` last, and drawing the two
  pictures from a set whose table is off so the bed is not attenuated twice. Recorded, not built.
- Volumetric media. The requirement's "and to volumetric media consistently" — the sample's device
  clouds are the seeded stand-in density, composed over the atmosphere's clear sky but not
  attenuated by the air in front of them. Recorded, not built.
- Moving the table's integration onto the device. It stays a processor integral packed into a
  storage buffer, as the sky view table and the cloud shadow field are.

## Decisions

### The engine's froxel table, sampled as a surface

`sky::AerialPerspectiveTable` is already the requirement's precomputed aerial perspective table and
already a `rendering::FroxelVolume`. A second, direction-and-distance table for the sample would
have been a second model of distance. The frame integrates the table for its own camera — the
basis from eye to target, the projection's own field of view and aspect — at 32 x 18 froxels and 32
slices out to the dome's 9 km. Only the eye's altitude reaches the atmosphere; the rays are built in
the camera's basis, which is the table's existing precision argument.

The table's own `sample()` returns the froxel whose far edge contains a depth, which is right for a
volume integrated front to back and wrong for a surface: a surface two metres away would receive
the first slice's full haze. `sample_at()` interpolates linearly in depth between far edges, from an
implicit slice at the eye (transmittance one, in-scattering zero), and bilinearly across the frame.
`cy/aerial_perspective.slang` is that function on the device, and `render.world_aerial_perspective`
compares every wall texel the device draws against `sample_at()` on the processor.

### One layout, defined once

`sky::pack_aerial_perspective()` writes the table as float4 words: five header words (the basis, the
fields of view, the volume's shape and planes, an enable flag, the radiance scale) and two words per
froxel. The shader module restates the layout in its header comment; the processor sampler reads
the table, the device reads the words, and the test holds the two together. An unbuilt table packs
the header alone with the enable word zero, which is what a frame with aerial perspective off binds.

### The sky and the air share their units

The dome's colour is its radiance divided by `4 * exposure` before the emissive path tone maps it.
`sky_radiance_scale()` in `stage.cpp` is that divisor, and the same function scales the table's
in-scattering when it is packed. A different divisor for the air would put a seam at the horizon
that the atmosphere did not.

### The dome's clear sky becomes the atmosphere's, and only when aerial perspective is on

With aerial perspective on, the stage rebuilds `sky::IncrementalSkyView::update_aerial` over the
world's tables every frame (row budget zero: this take moves the sun five degrees a frame, so a
row-budgeted table would draw a sky integrated for a sun that has already moved), samples it at each
dome vertex, and writes it into the dome's colour range. `world_visual.slang`'s cloud pass reads
that colour as the clear sky when `atmosphereSky` is set and composes its clouds over it; with the
flag clear it computes the stand-in gradient exactly as before. `verify_cloud_agreement` follows the
same switch.

`SkyViewTable` was not usable: it reads M7's isotropic multiple-scattering factor, which differs
from the tabulated multiple scattering the aerial perspective table integrates by up to a third near
the horizon. `update_tabulated` was not usable either, and the reason is the next decision.

### One step rule for the sky and the air

The first version drew the dome with `sky_radiance_tabulated()` and the horizon did not meet: a wall
800 km out was 0.17 brighter and bluer, after the tone map, than the sky above it. Both integrate
the same source terms from the same tables; they differ in QUADRATURE. `sky_radiance_tabulated()`
takes sixteen uniform steps and weights each by the transmittance at its far end; over a horizon
path a step near the ground is optically thick in blue, so that weighting drops most of the light
the first fifty kilometres scatter. The froxel table, with slices a few hundred metres thick near the
eye, did not lose it.

`sky_radiance_aerial()` and `AerialPerspectiveTable` now share one step rule
(`integrate_view_step()` in `tables.cpp`): each step integrated exactly for a source and an
extinction constant across it, `source * (1 - exp(-sigma * step)) / sigma` behind the transmittance
in front of it; the sky on steps growing as the square of the distance, the table on the froxel
slices. The seam then meets to within 0.006 over every column. `IncrementalSkyView::update_aerial`
fills the dome's table with it, spreading a full rebuild over the world's workers.
`sky_radiance_tabulated()` is left unchanged: the lighting integral, the exposure and every existing
capture are built on it, and changing it would move every frame of the take with aerial
perspective off.

### Off is exact, not close

`throughAir()` in `world.slang` returns `lit` untouched when the enable word is zero, without reading
the table, and the cloud pass takes the stand-in branch with the flag clear. The frame with
`--no-aerial-perspective` was compared PNG for PNG against the build of this tree's base
(`evidence/frame-identity.txt`), and `render.world_aerial_perspective` asserts every texel of the
off frame against the world's shaders pinned at 1fe6446.

### Set 0 grows a third binding

The push block is at the 128-byte limit, so the table is binding 2 of the lit pipeline's set 0,
beside the cloud shadow field and its placement, fragment stage only. `render.world_cloud_shadow`
restates the layout and now binds a switched-off table there; its assertions are unchanged.

## Backends

- **Vulkan** — the target on this host. `render.world_aerial_perspective` runs the shipped SPIR-V.
- **Metal** — `world_msl.h` and `world_visual_msl.h` are regenerated from the same sources; the set
  stays at `[[buffer(0)]]` and the push block at `[[buffer(1)]]`. Compiled, not run, on this host.
- **D3D12** — `samples/10-world` builds no D3D12 path (see `add-cloud-shadows`); the table adds one
  more SRV to set 0's descriptor table.

## Risks / Trade-offs

- **Per-frame CPU cost.** The clear-sky table (2 048 directions at 32 steps) and the volume
  (18 432 samples), both spread over the world's workers, are integrated every frame the camera or
  sun moves: 1.4 ms mean of `stage_build_ms` over the 192-frame day on this host, reported as
  `aerial_ms` and in the take's summary (`evidence/frame-identity.txt`).
- **The dome below the horizon is the planet's ground.** The dome reaches eighteen degrees below the
  horizon to cover the pixels past the world's edge; the atmosphere answers those directions with the
  ground albedo seen through the air, a grey-brown band where the stand-in drew more blue sky. It is
  the model's answer for a planet whose ground albedo is 0.1; a world with an ocean to the horizon
  would set it.
- **The world is small.** It is about a kilometre and a half across and the dome is at 9 km, so on a
  clear day the air adds a few per cent to the far hills rather than hiding them. That is the
  atmosphere's answer at that distance; the tests use walls hundreds of kilometres away to show the
  large-scale half of the requirement, and a thicker atmosphere shows more haze without a separate
  control.
- **The table ends at the dome.** Nothing past 9 km is visible in the sample; a surface beyond the
  volume's far plane reads its last slice.
