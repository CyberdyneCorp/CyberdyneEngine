# Design: many lights and area lights

## One record, seven kinds

`GpuLight` stays 64 bytes. An area light reuses the punctual fields: `spot_scale` and `spot_bias`
hold the half extents, `direction` the emitting normal (a tube's axis), `shadow_slot` an octahedral
tangent (`encode_area_tangent`), and `range` the reach — authored, or derived where the light's
illuminance falls to `kAreaLightReachIlluminance` (0.01 lux) so a cluster bound exists. The kind
word carries `kGpuLightTwoSided` (0x100) above `kGpuLightKindMask`; `kGpuLightBaked` (8) is the
placeholder a static light leaves. A frame without an area light uploads the records it always did.

```
render::LightDescription ──build_gpu_light──► GpuLight (64 B) ──► cluster lists / candidates
          │                                        │
          │ gi_gpu::append_gi_lights               └──► cy/light.slang: shadeAreaLight (LTC)
          ▼                                             lighting/area_shading.h: the host twin
     CyberGI emitter squares
```

## Shading

The diffuse term is the clipped polygon's form factor. The specular term is the LTC integral of the
polygon transformed by the table's inverse matrix for (roughness, N·V), times the table's
magnitude. Fresnel is evaluated at the half vector between the view and the integral's gather
direction (the vector form factor's direction), not by the split sum's hemisphere average: against
a Monte-Carlo integration of GGX (`integration.render_area_lights`) the split sum overestimated the
specular by 10% of its peak on average and 36% at worst; at the gather direction it is 3.2% and
11.1%. A sphere is the disc it subtends, facing the shading point; a tube is the rectangle of its
silhouette, its caps dark. The table is sampled at texel centres with a manual bilinear filter, so
the device reads exactly what the host's `LtcTable::entry` interpolates.

## Assignment on the device

`GpuLightAssignment` runs one compute dispatch per frame: each workgroup takes a tile, loads the
light spheres into group memory, keeps the lights whose sphere reaches the tile's four side planes
(and, for a spot, whose cone reaches the cluster), and writes each depth slice's nearest 64 to the
headers and indices the frame reads. The lists are therefore the host's lists less exactly the
lights the tile frustum excludes — `render.many_lights` (a) holds that set relation over every
cluster, and (b) that the two draw the same bytes. Lights are read from a device-local copy
(`FrameUpload::lights_buffer`), because a thousand host-visible reads per fragment cost more than
the shading.

Directional lights are named in the view block (`directional_lights`, up to three, flagged
`kCyDirectionalListed`) instead of found by walking every light: at a thousand lights the walk alone
cost 7.7 ms of a 1080p frame, the named list 0.59 ms for the whole lit shading (0.70 ms once the light loops were folded into one
call site each, which keeps the fragment the size of the frame before — see below).

## The stochastic path

```
alias table (power) ─► Candidates: 16 RIS candidates ─► reservoir ─► temporal reuse (cap 320)
                                                                     │
                     Spatial: 4 neighbours, balance-heuristic weights ◄──┘
                                    │
                                    ▼ one screen-space visibility ray for the selected sample
                     noisy diffuse irradiance ─► Accumulate ─► Variance ─► Filter × N (à trous)
                                    │
                                    ▼
             frame.slang: diffuse from the denoised irradiance, specular from the reservoir's light
```

The random numbers are a counter-based hash (lowbias32 of a Weyl sequence over pixel, frame and
draw), so a frame is reproducible. Constants travel as `uint4` words, never as float bits, so no
integer is flushed as a denormal. Spatial reuse uses the generalized balance heuristic over the
neighbours' targets: plain summation biased the estimate by −10.7% and a `1/Z` count by +5–14%
where neighbours' supports differ; with the heuristic the measured bias is 0.66% against a 1%
bound. The denoiser is `denoise::Denoiser`'s `StochasticDirect` signal transcribed, and case (f)
holds it to `Denoiser::denoise` over the device's own signal (worst 0.09%).

The selection is `LightingPathSelector`: it leaves the clustered path the frame the cluster bound
drops an assignment, or the clustered cost exceeds the arbiter's `MaterialEvaluation` allocation
(`shading_allocation_ms`); it returns only after a dwell and only when the predicted clustered cost
fits with a margin, so a scene at the boundary does not oscillate. The path is refused unless a
denoiser is available. `FrameAssembly::set_many_lights` applies the decision from the next frame.

## Lights for a moment

`TransientLights` is a fixed array of slots with generational handles (`kTransientLightIdBit` marks
their stable ids). Spawning writes a slot; `advance` ages them and frees the ones whose lifetime
ended; `append_to` writes the live lights, faded (none, linear, quadratic), into the frame's light
array. A directional light, a negative lifetime or a full pool are refused and counted. ABI 1.11's
five entries are its thin wrapper, and are refused in a fixed update: a resimulated tick must not
spawn its flash twice.

## Mobility

A static light leaves `baked_light_placeholder()` at its index — an index is the light's identity to
the lightmap's channels and to the reservoirs' history — so it is in no list and no alias table, and
the frame's loops skip `kLightBaked`. `render.area_lights` (d) shows a static light changes no texel
while stationary and movable lights light the same 18 170 texels.

## Off is the frame before

No area light, no LTC table, no device assignment and no stochastic path is the frame as it was:
`render.area_lights` (a) compares against `references/area_lights_off.png`, drawn by the frame
shader before this change, and `render.many_lights` (b) holds the frame drawn from the device's
lists to the frame drawn from the host's, byte for byte. The view block grows by two `uint4` (`lighting_control`, `directional_lights`), appended.

## One call site each

The forward fragment is a single inlined function, so every call of the area shading, the
directional shadow lookups or the punctual shading is a copy the driver compiles. The first version
shaded area lights in three places (the walk over every light, the cluster loop and the stochastic
path) and directional lights in two (by name and by walk): the fragment's SPIR-V grew from 38 194 to
59 376 words, and the driver's pipeline compile pushed `render.gi_gpu_frame`'s cases from 4.8 s to
5.3 s of CPU against their 5 s budget. `accumulateLights` now walks one loop for directional lights
(named or found) and one for local lights (every light, a cluster's list, or the reservoir's
selected light), each shading through one call site: 44 853 words, and the cases back at 4.86 s.
The cluster loop pays 0.11 ms of it at a thousand lights (0.70 ms against 0.59 ms). The arithmetic
and its order are unchanged, so the frame is the same bits: (a) by day and by night, (b) and (l).
