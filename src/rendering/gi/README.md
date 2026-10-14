# `src/rendering/gi/` — layer 4

CyberGI: the GI scene, the sparse distance field, the surface and radiance caches, the screen,
software and hardware tracing tiers with their sample confidence, the resolve, reflections on the
same infrastructure, the reflection probes, the offline path tracer, and the GI budget.

**Governed by**: `rendering-global-illumination`, at **Working** for M7. Tasks 9.1, 9.2 and 9.5.

## The files

| File | What it holds |
|---|---|
| `scene.h` | `GiMode`, `RadianceSource`, `RadianceSample`, `Surfel`, the error targets, `select_detail_level`, and `GiScene` — cell-scoped ingestion, eviction and attributable invalidation |
| `lighting.h` | the three seams the subsystems reach each other through — `Occluder`, `IndirectSource`, `RadianceLookup` — plus `SceneTracer`, `SceneHit`, `GiLight` (a point, a directional light, or a promoted emissive card), `emitter_form_factor` and the analytic `SkyTerm` |
| `distance_field.h` | camera-centred sparse clipmaps of bricks, per-asset fields composited by transform, sphere tracing and sky visibility |
| `surface_cache.h` | shaded radiance per surface card, the budgeted prioritised update (`SurfaceCache::select`), the one line the multi-bounce approximation is, and `SurfaceShadingBackend` — the seam a device shades the selected pages behind |
| `card_lighting.h` | the host oracle of the device surface cache: `ShadowMap`, `ShadowMapOccluder`, `CardGrid`, `CardSnapshot` and `CardGather` (issue #35) |
| `emissive.h` | emitters classified by radiant power and area: `classify_emitters`, the per-cell `EmissiveLights`, and `EmissiveReport`, which accounts for every watt |
| `gi_volume.h` | `GiVolume` and `GiVolumeSet`: per-region overrides of quality, probe density, update rate, intensity, tier preference and far-field distance, with priority and blend distance |
| `irradiance_volume.h` | a regular grid of SH L1 probes captured through the three seams, trilinear sampling weighted by validity, backface and a per-axis free-distance test, the update policy, and the texel layout `cy/frame.slang` reads |
| `proxy_scene.h` | `BoxProxyScene`: axis-aligned proxy boxes as a `SceneTracer`, `Occluder` and `RadianceLookup`, so a volume can be captured from blockout geometry |
| `radiance_cache.h` | adaptive geometry-aware probe placement, clipmap scrolling, three probe encodings, the visibility term, the scheduler that guarantees progress (`RadianceCache::select`), and `ProbeGatherBackend` — the seam a device gathers the selected probes behind |
| `tracing.h` | `ScreenTracer`, `SoftwareTracer`, `HardwareTracer`, `WorldTracer` and `TieredTracer` — one interface, cheapest sufficient tier, blended escalation |
| `reflections.h` | the roughness strategy table, the lobe distribution, and `ReflectionProbeSet` with box projection and amortised realtime capture |
| `screen_resolve.h` | the per-pixel resolve a frame shades with (issue #35, stage 4), the oracle `cy::rendering-gi-gpu` transcribes: `resolve_screen_pixel`, `ScreenResolve`, the history (`screen_history`), the half-resolution grid and its upsample (`ScreenGrid`, `screen_upsample`) |
| `mesh_cards.h` | a triangle mesh as the GI scene holds it: cards pooled per cell and facing (`append_mesh_cards`) and a signed distance grid from its triangles (`MeshDistanceField`) |
| `resolve.h` | `combine()` by confidence, `exclusion_for()` (the double-counting rule), the far-field ramp, and `ConvergenceTracker` |
| `budget.h` | the GI allocation, seven priced lever ladders, the declared reduction order, and importance |
| `bake.h` | the offline path tracer, ground truth, the reference comparison, and the seeds it leaves in the caches |
| `signals.h` | `StochasticSignals` — the FIVE stochastic signals `denoising` declares, produced from this system's own tracer and caches and each routed through `denoise::Denoiser`. M11.c task 2.5 |
| `classification.h` | `IlluminationClass` — Static, SlowDynamic, FastDynamic, TransientIgnored — derived from mobility and size with a per-object override, `GiObject`, the proxy fidelity of a skinned object, and `sphere_occlusion` |
| `media.h` | the transparency staging table, the reserved `RefractionSolver` and `CausticsSource`, thin transmission's back-face offset, and `MediaIndirectGrid` — the radiance cache's mean radiance a participating medium reads |
| `diagnostics.h` | `IlluminationQuery` (what lit a point, its probes and their leak suspicion, and a `DarknessCause`), `IlluminationRegionReport`, and `GiProfile` — the per-frame stage costs, rays per tier, hit rates and cost attribution |
| `debug_views.h` | the nineteen GI debug views, named as `render::DebugViewMode`'s, drawn into an `Rgba8Unorm` image the forward frame composites |
| `system.h` | the composition: the named subsystems, wired, and one frame |

## Five things worth knowing before changing anything here

**The software tier is the path that runs here, and that is a checkable fact rather than a choice.**
It used to be checkable in one line — `cy::rhi::Capability::RayTracing` was an enumerator nothing
set, so `AccelerationService` reported `Unsupported` on every device this engine could open. **M11.c
changed that half**: the Vulkan backend asks for `VK_KHR_ray_query` now and the RTX 5060 reports it,
so a service configured through `rt::service_config_for()` from a real device reports `Available`.
What did not change is where the rays go: `AccelerationService` still traces `cy::Bvh` on the
processor, and layer 4 may not name a Vulkan header anyway. A default-constructed service is still
`Unsupported`, which is what every suite in `tests/` uses and why the numbers below did not move.

The consequence is the default rather than a special case, and `tests/test_fallback.cpp` measures
what it costs. Over eighty points in a closed room, converged identically, the two tiers differ by

| | |
|---|---|
| mean relative difference, software against hardware | **0.0209** (tolerance 0.10) |
| an ordinary next frame moves the image by | 0.1903 |
| the frame that switches the profile off moves it by | 0.1842 |

(The first was 0.0751 and the third 0.1514 until stage 4 gave every brick of the field its far
corner — see "In the frame" below: the software tier now sees the same surfaces the hardware tier
does, so the two agree three times better and the switch is a smaller share of the frame's own
motion than before.)

The second and third lines are the criterion as a player meets it: turning ray tracing off moves the
image LESS than standing still does, because a hybrid renderer keeps converging after the world
stops and the tier is not what the eye sees moving. Every one of those numbers is identical in the
dev, release and debug profiles and across repeated runs — it is a computation, not a timing.

**A traced hit is a cache lookup, and the enforcement is that a tracer holds no material.** Both
world tiers take a `RadianceLookup*` and there is no path from either of them to a material graph.
That is what makes ray cost independent of material complexity, and it is why the hardware and
software tiers agree about what a surface looks like even when they disagree by a voxel about where
it is.

**The subsystems reach each other through `lighting.h`'s abstract seams and never by name.** The
surface cache does not know the radiance cache exists; it holds an `IndirectSource`. The radiance
cache holds a `SceneTracer` and a `RadianceLookup`. `system.h` is the only file that names two
concrete subsystems in one place, and it contains no illumination logic — which is the specification's
"WHEN the radiance cache is replaced or reworked THEN the GI scene, surface cache, tracers, and
resolve SHALL be unaffected" as a property of the link graph.

**Confidence is combined, never switched on.** `combine()` is a confidence-weighted mean. A maximum
would be shorter and would pop every time the ranking changed, which is every time the camera turns
past a screen edge. The same rule is why the tiered tracer blends a partial screen answer into the
world answer rather than choosing between them.

**The budget cannot see the frame, and it cannot relax on its own.** `GiBudget::update` takes this
system's own measured cost and there is no parameter a frame time could arrive through; relaxing
requires `permit_relaxation()` from the arbiter. Both halves come from the M7 arbiter spike
(`design.md` §2.6): a controller that relaxes on its own authority is spending a budget it cannot
see, and modelling that cost the spike's sweep 57 of 71 loads.

**The denoiser has producers now, and before M11.c it had none.** `denoising` declares five signals
and gives each its own configuration, and until this rung **nothing in this tree called
`Denoiser::denoise()` outside the denoiser's own two suites** — this module named `Denoiser` and what
it did with it was set its quality position from a budget lever. `signals.h` is the fix: one
stochastic sample per pixel per frame for each of indirect diffuse, indirect specular, ray-traced
shadows, ambient occlusion and stochastic direct, produced out of the tiered tracer and the surface
cache, and handed over across the plain-span seam the denoiser declares.
`Diagnostics::invocations` is the census that makes "which signals have a producer" an observation
rather than a grep, and `integration.render_gi_signals` reads it.

**The sky term is the atmosphere's, and this module still does not know that.** M11.c joined
[dependency cycle 2](../../../docs/roadmap/dependencies.md) in a third module —
`src/rendering/sky_illumination/`, which depends on `cy::rendering-gi` and `cy::rendering-sky` and is
the only library in the tree that depends on both. What this module gained is `set_sky_term()`, which
installs a term without paying for `configure()`, and `InvalidationCause::SkyChanged`, which is the
one cause `service_invalidations` does NOT hand to the distance field — a sun that rotated moved no
geometry.

## Emitters: a card or a light, by measurement

An emissive card lights its surroundings through the surface cache (`SurfaceCache::outgoing` adds
its emission), which is right for a large, dim emitter and the source of fireflies for a small,
bright one — and a card smaller than the cache's lookup cell is worse than noisy, because every
gather hit in that cell resolves to it. `classify_emitters` sorts emitters by radiant power and
area: one at least `promotion_power` strong and at most `promotion_max_area_m2` large is PROMOTED,
its emission taken out of the cache and each card handed to the direct lighting path as a `GiLight`
with `emitter_area`, which `direct_radiance` integrates in closed form (Lambert over the card,
clipped to the receiver's horizon) and shadows with one ray to a point `kEmitterShadowLiftMetres`
in front of it. `IlluminationSystem::ingest_cell` runs it and adds the promoted lights to every
frame's (`frame_lights()`); `scene().ingest_cell` without it is still every emitter through the
cache. Nothing is clamped, and `EmissiveReport::clamped_power` is checked to be zero.

Measured against the path tracer over the same cards (`integration.render_gi_emissive`; the
reference uses `PathTracer::set_emission_footprints`, which is off for every bake):

| a 4 x 2 x 4 m room, no light placed | energy error | per-receiver |
|---|---|---|
| 2.25 m^2 panel, promoted (22 receivers, 5 bounces) | 0.020 | mean relative 0.071 (reference noise) |
| the same panel, cache only, 64 gather rays | 0.086 | mean relative 0.140 |
| 30 cm lamp at radiance 400, promoted (25 floor cards, direct) | 0.005 | ratio variance 0.027 (reference noise), max ratio 1.33 |
| the same lamp, cache only, 32 gather rays | 0.067 | ratio variance 4.6, receivers from 0 to 7.8x: fireflies |

![The light arriving from the lamp over a 3 m patch of floor: the path tracer at 256 samples, the
promoted lamp, and the lamp left to the cache](../../../docs/design/images/gi-emissive-lamp.png)

*The light arriving directly from the 30 cm lamp over a 3 x 3 m patch of floor, 64 x 64 points:
left, the path tracer at 256 samples a point (its noise is what a stochastic estimator of a small
emitter looks like); centre, the lamp promoted, exactly the closed form; right, the lamp left to
the cache — every gather hit in the region of the ceiling it is the nearest card for returns its
full radiance, which is the blotches. The same quantities `render_gi_emissive`'s lamp case measures,
sampled densely once for the picture.*

## Moving objects, by illumination class

`IlluminationSystem::add_object` classes an object once (`classify_illumination`: `Static` mobility
is `Static`; a movable object under `transient_max_size_metres` is `TransientIgnored`, at least
`slow_dynamic_min_size_metres` is `SlowDynamic`, between is `FastDynamic`; `GiObjectDesc` carries an
override), and the class decides what `move_object` costs. Static and SlowDynamic objects are cells
of their own in the GI scene (`object_cell_id`), with their own field placement: a Static move
re-ingests them and invalidates now; a SlowDynamic move is coalesced and the next `update()` at least
`slow_dynamic_interval_frames` after the last flush re-ingests once, invalidating the union of where
it went. A FastDynamic object is never in a cache. Each frame it becomes proxy spheres — the coarse
proxy is one sphere of its bounds' volume; a skinned object within
`deformed_proxy_distance_metres * importance` of the camera uses the spheres `pose_object` handed over
— and `indirect_diffuse` takes the light each sphere covers away (`sphere_occlusion`, the exact
cosine-weighted solid angle, clipped at the horizon) and adds back what the sphere reflects of the
radiance cache's light on its near side; `indirect_specular`'s rays that meet a sphere before the
world see it. A TransientIgnored object is recorded and nothing else.

Measured (`integration.render_gi_classes`): a projectile crossing the room for thirty frames files
no invalidation and leaves the field exactly as a twin room without it; a 4.4 m vehicle moved every
frame for 32 frames is flushed 4 times — 8 records, every one naming it — instead of 32; a
character standing on the floor keeps between 0.74 and 0.81 of the indirect light under and beside
it where the path tracer, tracing a sphere of triangles in the same place, keeps between 0.65 and
0.85: mean difference 0.044 of the open light. Forty skinned walkers 60 m out are one sphere each;
the hero beside the camera is its four posed bones.

## Transparency and participating media

`transmission_support` is the specification's staging table. Thin transmission:
`SurfaceProperties::transmission` makes `indirect_diffuse` add `ResolveResult::transmitted` — the
radiance cache sampled about the reversed normal `kThinTransmissionBackOffsetMetres` behind the
surface, tinted — which a shader adds; with no transmission the resolve is bit for bit what it was.
Twelve backlit leaf points in the room: mean relative error 0.123 against the path tracer's
irradiance about the reversed normal. Volumetric media: `media_indirect` builds a
`MediaIndirectGrid` of the cache's mean radiance over the sphere (the six-direction average, exact
for SH L1) that `fog::FogIndirect` hands to the volumetric fog's march, on the host and the device;
over 32 points of the room with probes a metre apart the grid is within 0.125 (mean relative) of the
path tracer's six-hemisphere mean — the cache's spatial resolution is the error. True refraction and
caustics are DEFERRED: `RefractionSolver` and `CausticsSource` are declared, and
`set_refraction_solver` / `set_caustics_source` refuse with `ErrorCode::NotImplemented` naming it.

## Diagnostics: why is this dark, and what did it cost

`explain(position, normal, surface, settings)` resolves the point with the function
`indirect_diffuse` uses (`diffuse_samples`), so its `diffuse` is that answer bit for bit, and reports
every source's confidence and weight share, every contributing probe with its falloff, facing and
visibility terms (`RadianceCache::contributions`, the same function `sample` weights with), its age
and error, whether the field blocks the line from the probe to the point (a LEAK SUSPECT), the tier a
ray takes there, the region's convergence, and a `DarknessCause`: `Lit`, `Occluded`, `Unconverged`,
`MissingProbes`, `LowConfidence` or `Invalidated`. `explain_region` asks the same of a box, with the
pending and recent invalidations touching it. `profile()` is the frame the last `update()` began:
allocation and measured cost, wall time per stage (screen trace, world trace, probe update, surface
cache, denoise, reflection resolve — exclusive; the probe gather's rays are in probe update),
probes and updates, rays per tier, the screen hit rate, both caches' hit rates, and the frame's
invalidations attributed to the light, object or cell that filed them, costliest first.

**The leak query found a leak.** Four probes a metre behind the divider lit its face with 0.135 of
the probe weight: the visibility term averaged the wall's distance with the open air beside it.
`RadianceCache::visibility_weight` now also requires every axis the query lies within sixty degrees
of to reach the query on its own (the device transcribes it); that takes the face to 0.075. The
remaining suspects sit on the doorway's edge, where six axis distances cannot tell an edge from a
wall. The per-pixel resolve does not use `sample` for that reason: `sample_traced` traces each
probe through the field, and `integration.render_gi_screen` holds its 10 cm wall to the path tracer.

## Debug views

`render_gi_debug_view(system, view, camera, inputs, out)` draws each of `debug_views.h`'s nineteen
views from the functions the illumination is computed by, into an image the frame composites
(`lightmaps::GiDebugOverlayTexture`, `pipeline::kGiDebugOverlayView`); the two lightmap views are the
frame's own (`LightmapDensity`, `LightmapCharts`). `integration.render_gi_debug_views` holds each to
a committed golden image (`tests/references/gi_view_*.png`, `CY_RENDER_UPDATE_GOLDEN=1` rewrites
them) and requires every pair to differ; `render.lightmaps` (o) and (p) hold the frame's half.

![The nineteen GI debug views of the divided room](../../../docs/design/images/gi-debug-views.png)

## GI volumes

`IlluminationSystem::set_volumes` places `GiVolume`s; `region_settings(point)` is what they resolve
to there, and with none reaching a point it is the global settings, bit for bit. Per parameter,
lowest priority first, each volume moves the value toward its own by `weight * fade` (one inside,
falling to zero over `blend_distance_metres`); the tier preference switches at one half. Probe
density, update rate, quality and tier are decided when a probe is PLACED and recorded on it
(`Probe::update_rate`, `ray_scale`, `tier`), so `set_volumes` retires the probes in reach of a
volume that sets one of them; intensity and the far-field distance are resolved per sample in
`indirect_diffuse` and `indirect_specular`. The scheduler ages a probe at its rate and scales its
error by it, which on `integration.render_gi_volumes`'s two mirror-image interiors gives the marked
one 3.9 times the updates per probe within one budget.

## Irradiance volumes: the first slice that reaches a frame

`IrradianceVolume` is what `lighting.h` has always said an `IndirectSource` could be — "a baked
irradiance volume implements it too" — and what `exclusion_for()` and
`SurfaceProperties::irradiance_volume_radiance` were written for. It is also the first thing in this
module a frame shades with: `src/rendering/light_probes/` uploads its `pack_texels` and
`cy/frame.slang`'s `probeVolumeAmbient` transcribes its `ambient()`. `integration.render_gi_volume`
holds the capture to a CPU reference integration (worst 0.044 of the mean irradiance against the
reference's own SH L1 projection at 256 rays; 0.249 against the exact cosine integral, which is L1's
truncation and not sampling), colour bleeding near a red wall and not 14 m from it, the leak the
visibility term stops, and the update policy. It is not dynamic GI: see `irradiance_volume.h` for
what is not covered.

## On the device: the first slice (issue #35, stages 1 and 2)

`src/rendering/gi_gpu/` (`cy::rendering-gi-gpu`) holds the distance field and the surface cards on
a device and shades the cards in compute. This module stays device-free and gained only what that
needs from the host side: the field's change journal (`generation`, `last_changes`, `visit_bricks`),
a `revision` per surface page, the scheduler as a shared function (`SurfaceCache::select`), the
`SurfaceShadingBackend` seam `IlluminationSystem::set_surface_shading` installs, and
`card_lighting.h` — the host functions the dispatches transcribe, which `render.gi_gpu` holds them
to. Nothing is composited into the frame yet; see that module's README for what is measured.

Two host changes came out of running it. `CardGather` lifts each ray's origin one voxel off the card
before leaving at `t_min`, so a grazing ray cannot be answered by the surface it leaves. And every
card lookup — `SurfaceCache::radiance_at`, `CardSnapshot::find` and the shader — ranks candidates
with `card_lookup_prefers`: score, then alignment, then handle. Before it, a hit exactly on two
coincident cards (an edge) scored zero against both and went to whichever the index visited first.

## On the device: the radiance cache and the tiers (issue #35, stage 3)

The same split for the radiance cache: `ProbeGatherBackend` (`radiance_cache.h`) is the seam,
`RadianceCache::select` is the scheduler lifted out of `update()` unchanged — every live probe older
than the age cap first, then by priority, then by handle — and `RadianceCache::apply` is the one door
a backend's results come back through, refused for a probe whose `revision` moved while its gather
was in flight. `probe_ray_direction` is the golden-ratio sequence `gather_probe` traces, public so a
device traces the same directions to the last bit. `IlluminationSystem::set_probe_gather` installs
`cy::rendering-gi-gpu`'s `GpuRadianceCache`; null is the host gather, and with no backend the host
GI computes exactly what main computed (`openspec/changes/add-gpu-gi-radiance-tracing/evidence/
off-reference.txt`). `tests/test_probe_gather.cpp` holds the seam to the scheduler headlessly.

**The hardware tier has hardware behind it now — on the device.** `HardwareTracer` here still asks
`AccelerationService::trace`, which still traces `cy::Bvh` on the processor, and that is the oracle.
The device's hardware tier issues Vulkan ray queries over structures `cy::rendering-raytracing-gpu`
builds from the service's decisions, and `render.gi_gpu_tracing` holds it to this module's
`HardwareTracer` ray for ray. Its README has the numbers.

## In the frame: the per-pixel resolve (issue #35, stage 4)

`screen_resolve.h` is what a frame's pixels are lit by, and the oracle the device transcribes
(`cy::rendering-gi-gpu`'s `GpuScreenResolve`, held to it to about 1e-7 by `render.gi_gpu_resolve`).
A pixel takes the radiance cache traced to its own probes (`RadianceCache::sample_traced`: the
probes within one spacing, each kept only if the field lets a trace from the pixel reach it), one
cosine final-gather ray through the world tier, and the far-field sky, combined under
`exclusion_for()`'s mask; the rough specular takes the cache in the reflection direction and one
lobe ray. The shared `denoise::Denoiser` accumulates and filters both over the history
`screen_history` reprojects through `temporal-rendering`.

`integration.render_gi_screen` holds the frame to the path tracer and to the quality bars the issue
names, in the GI test room:

| | measured | asserted |
|---|---|---|
| per-pixel resolve against the path tracer, 240 points | 0.112 relative (the probes alone 0.436) | < 0.16, and < half the probes' |
| on the half-resolution grid, between grid samples | 0.148 | < 0.19 |
| a sealed room behind a 10 cm wall from a lit one | mean 2.6e-5 of the lit room's, worst pixel 7.6e-3 | < 1e-3, < 2e-2 |
| a light moved 4.9 m | within 10 % of the new converged frame in 11 frames | at most 14 |
| a converged still view | moves 0.0034 a frame (the one-ray signal under it 0.451) | < 0.01 |

**Three defects building it found, each with its regression.**

- *A brick's last voxel was flat.* A brick stored four samples an axis, so trilinear sampling past
  its fourth clamped and the last voxel of every brick was constant: a 10 cm wall at a brick face
  existed from one side only. Bricks store five (`kBrickSampleEdge`, `kBrickSamples`); `render_gi_
  pipeline`'s "a 10 cm wall at a brick face is in the field from both sides".
- *A traced probe saw through walls.* The cache's per-axis visibility distances do not stop a probe
  across a thin wall; `sample_traced` traces each probe from the pixel, and the device's surface
  bounce gathers cards through the field (`SurfaceBounce::Cards`) rather than through the probes.
- *A final-gather ray was weighted by what it met.* Combined by its own confidence, a ray that
  escaped to the sky outweighed one that met a stale card, so the accumulated estimate leaned toward
  the sky — the beauty shot's shadowed faces resolved two thirds of their bounce. The ray now counts
  in full and its confidence only decides how much of its value the cache's answer stands in for;
  `render_gi_screen`'s "a final-gather ray counts in full whatever it met".

`mesh_cards.h` builds a GI scene from meshes: `samples/12-beauty --gi on` is the first scene the
engine lights this way, and `just capture-beauty-gi` measures it against the path tracer over the
exact triangles (0.126 relative, where the flat sky term it replaces is 0.385).

## What is here at Working, and what is not

**Lightmap atlases are built, one module up** (M11.e, #36): `src/rendering/lightmap_bake/` packs a
level's UV2 charts into shared pages, path traces every texel through this module's `PathTracer`
over a triangle `SceneTracer` of its own, denoises, dilates and stitches, and seeds the caches here
from the same run; `src/rendering/lightmaps/` samples the result in the frame. This module gained
nothing but a corrected row: `exclusion_for(Baked, has_lightmap)` excluded only the dynamic
sources, so a lightmapped surface inside a volume resolved to the mean of the two — it now
excludes the volume (`render_gi`'s "a lightmapped surface in Baked mode takes its lightmap").
`GiLight::mobility` (`Static`, `Stationary`, `Movable`) is read by the lightmap bake alone, which bakes a
stationary light's shadow mask and rebakes a moved object's region incrementally; every consumer
in this module still shades every light.

The **screen tier's colour input is last frame's**, which is why its confidence is capped below one.
The **hardware tier executes on the CPU** in this module for the reason above; on a device it is
`cy::rendering-gi-gpu`'s (stage 3).

## What it does not depend on

No device, no render graph, no shader — the device half is `src/rendering/gi_gpu/`, which depends on
this module and not the other way round. Every tier is arithmetic over the GI scene, the field and the
caches; the hardware tier reaches a device through `cy::rendering-raytracing`, which does not name
one either. That is what lets every case in `tests/` run headless.
