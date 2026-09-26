# Design

## A regular grid of SH L1 probes, in the radiance cache's encoding

A probe stores the radiance arriving at it as SH L1, four coefficients per channel, encoded,
normalised and decoded by the radiance cache's own `encode_sample` / `normalise_payload` /
`decode_payload` with `ProbeEncoding::SphericalHarmonicsL1`. A second encoding would be a second
definition of what a probe means. L2 was rejected for this slice: nine coefficients per channel is
seven texels per probe instead of three and 56 more fetches per fragment, and the frame's ambient
term is a low-frequency signal. What L1 costs is measured below.

Probes sit on a regular grid (`origin + spacing * (i, j, k)`). Adaptive subdivision near geometry
and hand placement, which the requirement also names, are not built.

## Capture through the seams every GI consumer holds

A capture traces a fixed Fibonacci set of `rays_per_probe` directions (256 by default) through a
`SceneTracer`; a hit resolves through a `RadianceLookup` to the radiance leaving that surface, and a
miss takes the `SkyTerm`. These are the interfaces the radiance cache, the reflection capture and
the path tracer gather through, so a volume captures from `gi::BoxProxyScene` (proxy boxes: blockout
or bounding geometry) or from the composed `IlluminationSystem`'s world tracer and surface cache
without knowing which. `BoxProxyScene`'s lookup is Lambertian — `albedo / pi` times the shadowed
direct irradiance, plus `albedo` times an optional `IndirectSource` — and passing the volume itself
as that source makes each capture add a bounce.

A ray that leaves through the back of a surface marks the probe as inside geometry; more than
`inside_fraction` (a quarter) of them and the probe's validity is zero. Six more rays along the axes
record how far the world is in each direction.

`capture_all` stages every probe and commits them together, so a capture that reads the volume back
through its lookup does not depend on the order the grid is walked.

## Sampling: trilinear, then three factors

The query point is moved `normal_offset_metres` along the normal and located in the grid; the eight
surrounding probes are weighted trilinearly, then by

- **validity** (zero inside geometry);
- **backface**, `((dot(toProbe, n) + 1) / 2)^2 + 0.2` — DDGI's form: a probe behind the surface's
  plane is attenuated and never zeroed, so a surface whose every neighbour is behind it is dimmed,
  not black;
- **free distance**: the six axis distances bound a box of free space around the probe, and a query
  outside it on any axis weighs zero, softened over `visibility_slack_metres`.

The free-distance test is per axis rather than the radiance cache's cosine-weighted mean of the six
distances. The mean was tried first and leaked: a probe beside a wall with open sky above it averaged
the wall's 0.35 m with the sky's 40 m and let a query on the far side of the wall through at a third
of its unoccluded weight. Per axis, the same query reads zero.

The weights are renormalised; if none survive, or the query is outside the grid, the frame's flat
ambient answers, blended over one spacing at the boundary.

## The frame

`FrameViewData` gains `probe_volume_control` (slot, three counts), `probe_volume_origin` (camera-
relative origin, spacing) and `probe_volume_params` (coefficient scale, normal offset, visibility
slack), appended after `occlusion_control` so every field another committed module reads stays
where it was. `probeVolumeAmbient` in `cy/frame.slang` transcribes `IrradianceVolume::ambient`; the
forward fragment calls it only when the slot is set, and multiplies its result by albedo, material
occlusion and the AO visibility exactly as it multiplied the flat term.

The texture is `Rgba16Sfloat`, five texels per probe: red, green and blue coefficient vectors (so a
channel's irradiance is one dot product), then `(validity, +x, -x, +y)` and `(-y, +z, -z, 0)`.
Half floats because the frame's one sampler is linear and every device filters them; coefficients
are divided by a per-volume scale so the brightest fits. Sampling is at texel centres, so the filter
returns the texel and the eight-probe blend happens in the shader, where each probe can be weighted.

The texture is uploaded in a device frame of its own, like `MaterialTextureTable`'s, and only when
the volume's generation changed.

## The sky, and the frame's ambient convention

The capture's sky in the device suite is a uniform radiance equal to the frame's flat ambient
(`FrameAssembly::sky_irradiance()`, which the forward pass multiplies by albedo without a `1/pi`). A
uniform sky of radiance L decodes to L for every normal, so where a probe sees only sky the volume
reproduces the flat term exactly, and switching the volume on changes only what the scene around a
surface changes. Bounce light is computed physically (`albedo / pi * E`). The two conventions differ
by pi, and the volume inherits the frame's; correcting the frame's ambient convention is not this
change.

## Update policy

- `configure` leaves every probe invalid and queued: nothing is lit by a probe never captured.
- `capture_all` is the bake.
- `invalidate(region)` queues the probes whose cells touch the region, first in first out.
- `update` captures at most `probes_per_update` queued probes; under `Amortised` the rest of the
  budget refreshes the stalest probes round-robin, so every probe is refreshed within
  `ceil(probes / probes_per_update)` updates with no invalidation at all.

## What was measured

| | |
|---|---|
| uniform sky, worst probe error | 1.5e-4 of 0.4 |
| probe against the reference integration's own L1 projection | 0.044 of the mean irradiance (256 rays); 0.0093 at 1024 rays |
| probe against the exact cosine integral | 0.249 — L1 truncation (0.233 at 1024 rays, so not sampling) |
| host corner: redness of the ambient near / 14 m from the red wall | back wall 1.72 / 1.03, floor 3.47 / 1.06 |
| host hut: query inside, by the wall, with / without the visibility term | 0 / 0.043 |

On the device (`render.light_probes`, RTX 5060, Vulkan with validation and synchronisation
validation, zero validation errors):

| | |
|---|---|
| (a) the red wall's share of the redness (red wall minus the same corner with it painted white) | back wall 0.424 beside it, 0.047 across the room; cube 0.276 near, 0.038 far |
| (b) the unlit back wall, summed 8-bit channels | off 254 everywhere; on 351.4 at its foot, 304.2 above 2.5 m |
| (c) frame against `IrradianceVolume::ambient`, ambient-only frame, 60 842 pixels on +x, +y and +z faces | correlation 0.9927 in brightness, 0.9930 in redness |
| (d) off against absent; absent against the pre-change reference | 0 pixels; 0 pixels |
| (e) AO on over the volume: back wall within 0.25 m of the floor / in the open | 11.27 steps darker / at most 1 step |

## Mutation proofs

Each applied to the source, the suite rebuilt and run, and the file restored and md5-verified.

| Mutation | Red |
|---|---|
| a capture ray that hits a surface brings back the sky | host: reference integration, bleeding, bounce, leak, validity, blend, GI room |
| the free-distance term skipped | host: `a probe inside geometry weighs nothing, and a probe beyond a wall does not leak` |
| each sample projected onto the mirrored direction | host: reference integration, bleeding, bounce |
| the update ignores its budget | host: the update policy |
| the shader computes the volume's term and returns the flat one | device: (a), (b), (c), (d)'s control |
| the shader's SH basis reads `normal.x` where it should read `normal.y` | device: (c) |
| the path with no volume scaled by 5% | device: (d), through the committed reference only — absent and off stay equal to each other, which is why the reference exists. At 1% the change is below one 8-bit step on every pixel of this scene and nothing can see it. |
| ambient occlusion skipped when a volume is attached | device: (e) |

## Backends

Vulkan is built and tested. The frame's MSL is regenerated and committed; `just build-shaders
--strict src samples` compiles `frame.slang` for Vulkan, Metal and D3D12 with no refusal. Metal and
D3D12 are not run on this host.
