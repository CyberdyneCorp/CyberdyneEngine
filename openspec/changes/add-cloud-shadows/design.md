# Design

## Context

Three pieces of `Cloud shadows` were built and never joined:

| piece | where | state before this change |
|---|---|---|
| producer | `sky::CloudShadowField` (`src/rendering/sky/cloud_shadows.h`) | marches `CloudField` from the ground to the top of the deck along the sun, writes a `Presentation` `UNorm8` field into `cy::environment` at 128 m and 1024 m, declares four consumers; tested by `integration.render_sky_fields` |
| device sampler | `cy/cloud_shadow.slang` | `cyCloudShadowAt` / `cyCloudShadowAttenuate` through the GPU scene's bindless table; compiled by `smoke.material_slang`, imported by no shader |
| illumination library | `rendering::apply_cloud_shadows` (`src/rendering/lighting`) | attenuates directional lights at one position; called only by its own test |

`samples/10-world` — the one frame in the tree that draws terrain, foliage and water under a weather
driven sky — lit its whole world with `SkyLighting::sun_illuminance`, which is the clear sun times
ONE cloud march taken from the camera's focus. A cloud over the camera darkened every hill; a cloud
over a hill darkened nothing.

## Goals / Non-Goals

**Goals:**

- Every surface the world frame lights with the sun reads the field at its own position.
- The field attenuates the direct sun and nothing else.
- A frame without cloud shadows, or under a clear sky, is bit-identical to the frame before.
- The shadow is the cloud the sky lights with, at the same time, advected by the same wind.
- Every one of those four is a test that has been seen failing.

**Non-Goals:**

- The engine forward frame (`cy/frame.slang`). It has no environment-field binding at all — set 0
  carries the globals, the material table and its sampler, and `cy/field.slang`'s bindless table
  at set 0, binding 3 is bound by nothing in `rendering::pipeline` — and it draws no world today.
  Adding the binding means a new set-0 layout in `frame_pipelines.h`, a write in
  `frame_bindings.cpp`, new Metal argument-buffer indices and regenerated `frame_spirv.h` /
  `frame_msl.h`, all of which the parallel ambient-occlusion change is also editing. Recorded as
  what remains, not built.
- Moving the producer onto the device. `CloudShadowField` stays `FieldProduction::Cpu`.
- Changing the sample's device sky dome. It is still the seeded stand-in density
  `close-world-frame-budget` introduced (see "What the dome draws" below).

## Decisions

### The field is sampled per fragment, in the one lit path

`world.slang`'s fragment stage draws terrain, water and foliage with one Lambert term and one
specular lobe. Sampling there covers all three consumers the requirement names for surfaces in one
place, per pixel, through `cy/cloud_shadow.slang`'s `cyCloudShadowAtImage`. A per-vertex stream was
rejected: it would need a third vertex binding on both the static and the dynamic buffers and a
CPU write for every foliage vertex, for a field whose cell is 256 m.

The image and a one-element placement buffer (origin offset, enable flag) are set 0 of the lit
pipeline, fragment stage. The push block is already at the 128-byte portability limit, so the
placement cannot live there. The set is declared BEFORE the push block in the Slang source because
Slang numbers Metal buffer slots in declaration order and the Metal RHI binds set N at buffer N and
the push block at buffer `set_count`.

### The sun is placed before the clouds when the field is read

`SkyLighting` gains `clear_sun_illuminance` — the value `compose_sky_lighting` already computed
before multiplying by the march at the viewer. With cloud shadows on, the lit path is given the
clear sun and the field does the attenuation; with them off it is given `sun_illuminance` exactly as
before. The frame assembly's own light list keeps `sun_illuminance`, so nothing else in the frame
changes.

### Off and clear are exact, not close

The fragment computes `direct = sun * sunThroughClouds(p)`. Off, `sunThroughClouds` returns the
literal 1.0 without reading the field, and `sun * 1.0` is `sun` in IEEE arithmetic. Under a clear
sky the producer writes `exp(0) = 1` into every cell, `UNorm8` 255, which decodes to exactly 1.0;
and `sun_illuminance` under a clear sky is `clear_sun * 1.0`. `render.world_cloud_shadow` asserts
bit-identity for both on every texel, and `samples/10-world --no-cloud-shadows` was compared PNG
for PNG against the build before this change: 192 of 192 frames identical
(`evidence/frame-identity.txt`).

### One cloud, one time, one wind

`World::update_cloud_shadow` passes `CloudShadowField::update` the same `cloud_field_` whose layers
`drive_cloud_layers` sets from weather's `CloudDrive` every frame, the same `seconds_` the sky
composition is given, and the sun `solve_celestial` placed. `cloud_density` advects by each layer's
wind times that time, so the shadow drifts with the weather's wind by construction. The field is
rewritten every frame: a frame of this take is two simulated minutes, far above the lever's 8 Hz
period, and a field on a wall clock would lag its clouds by kilometres.

The field is written around the world's centre, not the camera, so the regional image keeps one
origin for the whole take. 256 m cells with a 1 km radius straddle the tile boundary at the world's
corner, so the regional level is the four 4 096 m tiles about it — the world and at least 2.5 km of
sea on every side — at eight steps: 2 048 cells a frame with the macro level. A 4 km radius at
twelve steps (3 328 cells) was measured first and cost 5 ms of `sky_ms`; this costs 1.3 ms
(`evidence/frame-identity.txt`).

### What the dome draws

The sample's sky dome is `world_visual.slang`'s `composeClouds`, a seeded value-noise stand-in that
`close-world-frame-budget` moved onto the device; it is not `CloudField`. The shadows, the sun's
cloud attenuation and the ambient term all come from `CloudField`, as they did before. So the sky
and the ground agree in the ENGINE — `integration.render_sky_fields` proves `compose_sky()`'s cloud
transmittance toward the sun equals the field at the same ground point — and the sample's DOME is
the one piece of the picture that does not. Making the dome march `CloudField` on the device is the
remaining step and is recorded in the sample's README.

## Backends

- **Vulkan** — the target on this host. `render.world_cloud_shadow` runs the shipped SPIR-V.
- **Metal** — `world_msl.h` is regenerated from the same source; the fragment reads the set at
  `[[buffer(0)]]` and both stages read the push block at `[[buffer(1)]]`, the RHI's convention.
  Compiled, not run, on this host.
- **D3D12** — `samples/10-world` does not build a D3D12 path: its frame ends in the tonemapping
  resolve, a fullscreen pass D3D12 cannot compile yet (the M11.d gap), and the sample links no
  `cy::rhi-d3d12`. What cloud shadows add to that list is a DXIL payload of `world.slang` generated
  from the same source with Slang's pinned DXC, as `first_light_dxil.h` is, and a root signature in
  which set 0 is a two-SRV descriptor table visible to the pixel stage.

## Risks / Trade-offs

- **Per-frame CPU cost.** The producer marches 2 048 cells of 8 steps every frame on the main
  thread: 1.3 ms of daytime `sky_ms` on a loaded Linux host. The Apple M3 Pro take that holds the
  16.7 ms gate had 1.8 ms of margin and was not re-measured here; the headless take does not
  produce the field at all. `CloudShadowField::update` spreading its cells over the world's
  workers is the lever if a device take needs it back.
- **A shadow edge at the regional image's border.** Beyond the four tiles (±4 096 m of the world's
  corner) the fragment reads the declared default, full sun. The orbit sees no ground there.
