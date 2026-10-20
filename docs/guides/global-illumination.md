# Global illumination in CyberEngine

A walkthrough for a contributor or an agent: how CyberGI is put together, how its host and device
halves meet, how the tracing tiers are chosen, how a frame's pixels are lit by it and what that
costs, how emissive surfaces light a scene, how GI volumes override the global settings in a region,
how every result is held to the host oracle and the path tracer, and what is not built yet.

**Governed by**: [`rendering-global-illumination`](../../openspec/specs/rendering-global-illumination/spec.md)
and [`ray-tracing-infrastructure`](../../openspec/specs/ray-tracing-infrastructure/spec.md). The
module READMEs below are the reference; this guide is the route through them. Issue #35 is the plan
for the device half, in five stages.

| Where | What |
|---|---|
| [`src/rendering/gi/`](../../src/rendering/gi/README.md) | The host system and the oracle: the GI scene, the distance field, the surface and radiance caches, the tiers, the resolve, the bake's path tracer, the budget |
| [`src/rendering/gi_gpu/`](../../src/rendering/gi_gpu/README.md) | The device half: the scene upload, card lighting, the radiance cache's gather and query, the tiers, the per-pixel resolve and denoiser, the budget link |
| [`src/rendering/raytracing/`](../../src/rendering/raytracing/README.md) | The acceleration service: every decision about structures, and the mirror seam |
| [`src/rendering/raytracing_gpu/`](../../src/rendering/raytracing_gpu/README.md) | The service's structures built on a device |
| [`src/backends/rhi/`](../../src/backends/rhi/README.md) (`acceleration.h`) | Acceleration structures in the RHI, and the refusal of a device without them |
| [`src/rendering/light_probes/`](../../src/rendering/light_probes/README.md) | The irradiance volume, which the dynamic GI blends with under one rule |
| [`samples/12-beauty/`](../../samples/12-beauty/README.md) (`gi.h`) | The beauty shot lit by all of it: `--gi on`, `just capture-beauty-gi`; a sunlit room, a workshop hall and the shot beside the path tracer, `just capture-gi-interior`; the hall in motion, `just capture-gi-hall-video` |
| [`src/rendering/gi_gpu/`](../../src/rendering/gi_gpu/README.md) (`gpu_frame.h`, `gpu_debug_view.h`) | The device GI as one frame feature a host attaches, and the nineteen debug views drawn by the device — what the editor's viewport and a game's frame are lit and inspected with (section 6g) |
| [`samples/13-rts-api/`](../../samples/13-rts-api/README.md) (`host/illumination.*`, `host/render.*`) | A Swift game's frame lit by the device GI, the game tuning it through ABI 1.9 (section 6g) |
| [Lighting authoring](lighting.md) | Lightmaps and baked volumes, which this system combines with rather than replaces |

## 1. One system, two halves, one oracle

`IlluminationSystem` (`gi/system.h`) wires named subsystems that meet only through the abstract
seams in `gi/lighting.h`. Each subsystem that does arithmetic has a host implementation, and each one
issue #35 moves to the device keeps that host implementation as its **oracle**: the device replaces
the arithmetic behind a seam, the host keeps every decision, and a suite runs both over one scene and
compares buffers.

| Stage | On the device | Seam | Suite |
|---|---|---|---|
| 1 | the distance field's clipmaps and the surface cards, uploaded incrementally | — (a mirror of `DistanceField`) | `render.gi_gpu` |
| 2 | the surface cache's card lighting | `SurfaceShadingBackend` | `render.gi_gpu` |
| 3 | the radiance cache's gather and query; the screen, software and hardware tiers; the acceleration structures | `ProbeGatherBackend`, `StructureMirror` | `render.gi_gpu_tracing` |
| 4 | the per-pixel resolve, the denoiser, the composite into the frame | `FramePassKind::ScreenSpaceGi`'s producer | `render.gi_gpu_resolve`, `render.gi_gpu_frame` |
| 5 | the device cost in `GiBudget`, the levers onto the device, the arbiter's ladder | `GpuGiBudget`, `apply_levers` | `render.gi_gpu_resolve` |

**Off is the frame it was.** With no `ScreenSpaceGi` producer attached, or attached with its words at
"none", the forward frame is byte for byte the frame before stage 4 (`render.gi_gpu_frame` (a), and
the committed references of the pipeline, ambient occlusion, fog and lightmap suites); the beauty
shot without `--gi on` is M11.c's still byte for byte (`just capture-beauty-gi`).

## 2. Putting the caches on the device

The choice is one installation per cache, and a null installation is the host again:

```cpp
using namespace cy::rendering;
gi_gpu::GpuGiScene scene;                              // the field, the cards, the lookup grid
scene.create(allocator, device, {});

gi_gpu::GpuSurfaceShading shading;                     // stage 2
shading.bind(scene, system.field(), system.surfaces().lookup_radius());
system.set_surface_shading(&shading);

rt_gpu::DeviceStructures structures;                   // stage 3: refuses without ray queries
if (structures.create(allocator, device, {})) {
    system.acceleration().set_mirror(&structures);
}
gi_gpu::GpuRadianceCache radiance;                     // stage 3
radiance.create(allocator, device, scene, {});
radiance.bind_field(system.field());
radiance.bind_hardware(&system.acceleration(), structures.created() ? &structures : nullptr);
system.set_probe_gather(&radiance);

// Each frame:
system.update(context);        // the host decides: placement, scroll, selection, budgets
shading.declare(graph);        // the cards it selected
structures.declare(graph);     // the structures the service built, refit or moved
radiance.declare(graph);       // the probes it selected, through radiance.world_tier()
// execute the graph; the next update() applies what came back
```

Every backend is **asynchronous by construction**: `submit` takes this update's selection, the work
runs when the graph executes, and the cache applies the results at the start of its next update. A
result for a page or a probe whose `revision` moved while it was in flight — it was invalidated,
retired or reseeded — is dropped, so a stale answer never undoes an invalidation.

## 3. The tiers, and the one predicate that chooses between them

`TieredTracer` (`gi/tracing.h`) is the one interface a consumer asks for radiance along a ray: the
**screen tier** first, against the depth buffer and last frame's colour; the **world tier** only when
the screen's confidence is below the escalation threshold and the budget allows; and the two
**blended** by the screen's confidence rather than switched. The world tier is either:

- **software** — a sphere trace of the sparse distance field, which every device can run; or
- **hardware** — a ray query over the acceleration service's structures.

Either way **a hit is a surface-cache lookup and never a material evaluation**; the tracers hold a
`RadianceLookup` and have no path to a material, which is why ray cost does not follow material
complexity and why the two tiers agree about what a surface looks like even when they disagree by a
voxel about where it is.

On the device, `GpuRadianceCache::world_tier()` is hardware exactly when the device reports
`Capability::RayTracing`, the service is active and its top level is built — the host's
`HardwareTracer::available()` with a device behind it. A device without ray queries and a profile
that switched them off therefore run one path, and the suite holds them to the same image bit for
bit. `GpuTierTracer` runs a batch through the tiers on their own (`GpuTierMode::Screen`,
`ScreenAccelerated`, `World`, `Tiered`).

The screen tier's device march is the host's fixed-step march with one addition: a
**nearest-depth pyramid** that proves runs of steps empty — in front of everything they could touch —
and skips them. It changes no answer (the suite compares with and without it bit for bit) and pays
where a ray crosses open screen. The engine's HZB keeps the furthest depth and proves occlusion; the
march needs the dual, so the screen tier builds it.

## 4. A frame's pixels: the resolve, the denoiser and the composite

The host oracle is `gi/screen_resolve.h`; the device is `gi_gpu::GpuScreenResolve`, the producer of
the forward frame's `ScreenSpaceGi` stage. A frame of it is seven stages; the resolve is the final
gather the lever names — the screen probes by default (below), the per-pixel gather this table
describes otherwise:

| Stage | Host function | What it does |
|---|---|---|
| unpack | — | the `DepthNormal` prepass as surfaces: position, view depth, normal, roughness |
| reproject | `screen_history` | each pixel's history pixel through `temporal-rendering`'s motion and classification; up to four invalidated boxes keep none |
| resolve | `resolve_screen_pixel` | the radiance cache traced to its probes (`sample_traced`), one cosine final-gather ray through the world tier, and the far-field sky, combined under `exclusion_for()`; the rough specular |
| accumulate, variance, filter | `Denoiser` | the shared denoiser, one dispatch a pass, its configuration read off the host denoiser; with no cascade to run — the screen probes' default — one pass, `cyGiDenoiseTemporal`, accumulates both signals and publishes them |
| publish | `screen_upsample` | the grid back to the frame's pixels, joint-bilaterally, into two `Rgba16Sfloat` targets |

Attaching it to an assembled frame is three lines in the frame's hooks, and the composite is the
frame's own: `cy/frame.slang` samples the two targets through two slots of its texture table where
the irradiance volume's ambient sits, under the same double-counting rule (`frameGiAmbient`,
`frameGiAdmits`), with the specular through the split-sum environment BRDF:

```cpp
description.global_illumination = true;                       // AssemblyDescription
// before assemble, each frame:
system.update(context);
resolve.sync(system.radiance());
resolve.set_cache_frame(frame);
resolve.begin_frame(view, frame);                             // camera, unjittered world-to-clip, ...
assembly_view.screen_space_gi = resolve.import_diffuse(graph);
assembly_view.screen_space_gi_specular = resolve.import_specular(graph);
sinks.screen_space_gi = resolve.stage();
// once the upload exists:
resolve.set_jitter(jitter);
gi_gpu::write_gi_control(diffuse_slot, specular_slot, mode, upload.view.gi_control);
```

**The final-gather ray counts in full, whatever it met.** One ray a pixel is an estimate of the
hemisphere only on average over the directions drawn, so its weight in the combine must not depend
on the direction: its confidence decides only how much of its *value* the cache's answer stands in
for (a stale card, a hit no card covers, a back face). Weighting the ray by its own confidence — as
the resolve first did — let every ray that escaped to the certain sky outweigh one that met a card
shaded a few frames ago, and the beauty shot's shadowed faces resolved two thirds of their bounce.
`integration.render_gi_screen`'s stale-wall case holds every frame to the rule.

**The grid.** `set_divisor(2)` resolves, traces and denoises every other pixel each way and
upsamples against the full-resolution normals and depths; it is what `TracingResolution`'s first
step buys, because at 1080p the cache query and the denoiser cost more than the rays.

**The budget (stage 5).** `GpuGiBudget::observe` takes the stages' timestamps; `measured_ms()` is
what `FrameContext::measured_gi_ms` receives, and `GiBudget` tightens on it in its declared order.
`apply_levers` turns `TracingResolution` into the grid and the ray density and hands the denoiser's
quality to the resolve. Under the screen probes the ray density is the fraction of probes that
trace a frame (`screen_probe_traces`): a probe's trace costs mostly what it does once, so half its
rays cost 0.92 of all of them where half the probes cost half; one that does not trace keeps the
probe it stands for. `submit` reports the cost and the rung of the arbiter's GI ladder the levers
add up to; `apply` hands back the allocation and, only when granted, one relax step.

**The screen probes: the default gather.** `gi/screen_probes.h` (`ScreenProbeGather`) is the
resolve stage the frame runs unless `ScreenResolveSettings::gather` says `GiGather::PerPixel`:
probes on a screen grid in the class of Lumen's Screen Probe Gather. A frame of it is three device
passes in place of the per-pixel resolve, each the transcription of its host function:

| Pass | Host function | What it does |
|---|---|---|
| place | `ScreenProbeGather::place` | a uniform probe at the centre of every 8- or 16-pixel tile (`ScreenProbeSettings::spacing_pixels`); an adaptive probe on each half-tile cell whose centre the four uniform probes cannot interpolate — their bilinear, plane-distance and normal weights sum below one half: a depth step, a crease, the sky's edge |
| trace | `trace_probe` | 32 rays a probe through the world tier, one workgroup a probe; directions from a defensive mixture — a 4x4 guide over the hemisphere's equal-area square weighted by the probe's previous radiance (or the radiance cache's), beside uniform and cosine shares — each weighted by one over the mixture's density; the estimate in nine RGB second-order harmonics, blended with the history of the one previous probe that stands for it best where it lands |
| filter | `filter` | the probe-space denoiser: each probe's harmonics with the uniform probes' of the 3x3 tiles about it, by a tent over the tiles, a world reach of a tenth of the probe's depth and the plane and normal weight, into the coefficients the pixels read; the probe keeps its own as its history |
| integrate | `integrate` | every cell of the resolve grid from the uniform and adaptive probes about it, by bilinear, plane-distance and normal weights, each probe's harmonics evaluated about the cell's own normal; the far sky beside it; the rough specular from the same harmonics in the reflection direction |

Harmonics rather than an octahedral map: what a pixel reads is irradiance about its own normal, and
the clamped cosine attenuates every band past the second to at most 1/24, so nine coefficients a
channel (27 floats) hold it where an 8x8 map is 192, evaluated in closed form per pixel. The
estimate is unbiased whatever the guide draws (an open sky to 0.3 %); the guide draws at most 0.9 of
the rays, because a cell it weighs zero is never drawn. A probe's rays leave its surface one
clearance away from any OTHER surface too, along the field's gradient (DDGI's relocation, for one
origin): a probe on a floor two centimetres from a 10 cm wall, lifted straight up, stepped inside
the wall and read the lit room's card, and one on a room's edge sent its rays out through the
ceiling to the sky.

**The probes are denoised in probe space** (`ScreenProbeDenoiser::ProbeSpace`, the default): the
filter above, then per pixel the shared denoiser's accumulation alone — no variance, no a-trous
pass (`screen_signal_config`), one device pass — with the history cut where the integrated signal
strays from it by more than a fifth (`denoise::SignalConfig::history_rejection`). The integrated
signal is an average of 32 rays over four probes and moves a tenth as much from frame to frame as
the one-ray signal, so the spatial cascade bought little and cost most of the frame: at 1080p the
shared denoiser was 3.1 ms of the probes' 5.5 (hardware), the probe-space one is 0.6 ms of 2.0. The
cut is what settles a moved light: an accumulation weighs each frame by one over the frames since
its history was last rejected, so after an invalidation it averaged in every frame the caches spent
converging (14 frames through the software tier); cut where the signal leaves its history, it
follows them (9). `ScreenProbeDenoiser::Shared` keeps the previous path selectable, unchanged.

**A scene from meshes.** `gi/mesh_cards.h` builds what the GI scene needs of any mesh: cards pooled
per world cell of a spacing and per facing (so a cylinder's slivers are not thousands of cards), and
a signed distance grid in the mesh's own space for the field to place by transform. The beauty shot
(`samples/12-beauty/gi.h`) is built that way from its six primitives and thirty-one instances.

## 5. Emissive surfaces

A surfel's `emission` is radiance. Ingested through `IlluminationSystem::ingest_cell`, every emitter
— the cards of one (instance, material) — is classified by `classify_emitters` (`gi/emissive.h`):

| | radiant power `pi * Y(L) * A` | area | what happens |
|---|---|---|---|
| cache only | below `promotion_power` | any | lights its surroundings through the surface cache's bounce |
| cache only | any | above `promotion_max_area_m2` | the same: large enough for the gather to sample densely |
| promoted | at least `promotion_power` | at most `promotion_max_area_m2` | its emission leaves the cache; each card becomes a `GiLight` with `emitter_area` |
| demoted | promoted, but past `max_promoted_lights` for the cell | | back in the cache, its power reported |

A promoted card is integrated by every consumer of `shaded_direct` — the surface cache, the device
card shading, the path tracer's next-event term, the irradiance volume capture — with
`emitter_form_factor`: Lambert's closed form over the card clipped to the receiver's horizon. It is
deterministic, so it has no variance at all, and its energy is exact rather than estimated. The
shadow is one ray to a point 30 cm in front of the card (`kEmitterShadowLiftMetres`).

**Why promote.** A small bright emitter left to a gather is hit by a few rays and returns an
enormous value when it is: fireflies. In the surface cache it is worse, because a card smaller than
the lookup cell answers every gather hit in that cell, so its energy is inflated by the cell's area
over its own. `integration.render_gi_emissive` measures both on a 30 cm lamp: promoted, the floor's
direct light is within 0.5% of the path tracer with a per-receiver ratio variance of 0.027 (the
reference's own noise); left to the cache, the variance is 4.6 and the receivers range from
nothing to 7.8 times their reference.

**Energy is never clamped.** `EmissiveLights::report()` gives `total = cached + promoted + demoted`
and `clamped_power`, which is zero and is checked.

## 6. GI volumes

A `GiVolume` (`gi/gi_volume.h`) is a box, a priority, a blend distance, a master weight, and the
parameters it SETS (`sets`, a bit per `GiVolumeParameter`):

| parameter | what it changes | decided |
|---|---|---|
| `Quality` | a probe's rays per update: Low 0.5x, Medium 1x, High 2x, Epic 4x | at probe placement |
| `ProbeDensity` | above one, probes at full density whatever the geometry; below one, the sparse lattice only | at probe placement |
| `UpdateRate` | how fast a probe ages in the scheduler, and how much its error weighs | at probe placement |
| `TierPreference` | `SoftwareOnly` keeps rays off the hardware tier; `WorldOnly` skips the screen tier | at placement (probes) and per sample (specular) |
| `DiffuseIntensity`, `SpecularIntensity` | scale the resolved indirect term | per sample |
| `FarFieldDistance` | where the far-field ramp begins | per sample |

Resolution is per parameter and per sample: starting from the global settings, the volumes that
reach the sample are applied from the lowest priority to the highest, each moving the value toward
its own by `weight * fade`. A higher priority therefore dominates, and the blend band interpolates
from the global value rather than switching. The tier preference switches at weight one half.
`set_volumes` validates every volume first and refuses the whole set on one bad volume, then
retires the probes in reach of every volume that sets a placement parameter, so the next update
places them again.

```cpp
GiVolume interior;
interior.bounds = Aabb::from_min_max({-4, 0, -3}, {4, 3, 3});
interior.priority = 10;
interior.blend_distance_metres = 1.0F;
interior.sets = gi_volume_bit(GiVolumeParameter::ProbeDensity) |
                gi_volume_bit(GiVolumeParameter::UpdateRate) |
                gi_volume_bit(GiVolumeParameter::Quality);
interior.probe_density = 2.0F;
interior.update_rate = 4.0F;
interior.quality = GiQuality::High;
gi.set_volumes({&interior, 1});
```

With no volume reaching a sample, everything resolves exactly as it did before volumes existed —
`integration.render_gi_volumes` checks that bit for bit.

## 6b. Moving objects, transparent ones, and fog

**Class an object once and let the class price its moves.** Register what moves with
`IlluminationSystem::add_object` and move it with `move_object`:

```cpp
gi::GiObjectDesc car;
car.id = 9;
car.mobility = gi::ObjectMobility::Movable;   // with its size, this makes it SlowDynamic
car.local_bounds = cy::Aabb::from_center_extents({}, {2.2F, 0.8F, 1.0F});
car.transform = placed;
car.field = &car_field;                         // optional: its own distance field
car.surfels = {car_cards.data(), car_cards.size()};
(void)system.add_object(car, frame);
// every frame it drives:
(void)system.move_object(9, transform, frame);  // coalesced; flushed once per interval
```

| Class | Derived when | What a move costs |
|---|---|---|
| `Static` | static mobility | its cards and field move now; the region is invalidated now |
| `SlowDynamic` | movable, at least `slow_dynamic_min_size_metres` (2.5) | one invalidation of the union of where it went, at most every `slow_dynamic_interval_frames` (8) |
| `FastDynamic` | movable, in between | nothing: it is proxy spheres that darken the indirect light under them and show in reflections |
| `TransientIgnored` | movable, under `transient_max_size_metres` (0.25) | nothing at all |

`has_class_override` wins over the derivation. A skinned `FastDynamic` object hands its posed spheres
to `pose_object` each frame; near the camera (`deformed_proxy_distance_metres * importance`) they are
its proxy, beyond it one sphere of its bounds' volume is.

**Thin transmission is one field on the surface.** Set `SurfaceProperties::transmission` to the
tinted fraction a leaf lets through and add `ResolveResult::transmitted` to the shaded colour; it is
the back face's indirect light. Zero is the opaque resolve, bit for bit.

**Fog reads the radiance cache.** `system.media_indirect(bounds, spacing, fallback, grid)` samples the
cache's mean radiance on a grid; wrap it in a `fog::FogIndirect` and set `FogLight::indirect`, and the
march (host and device) scatters it in place of the ambient term. No grid is the fog as it was.

True refraction and caustics are deferred: `set_refraction_solver` and `set_caustics_source` refuse.

## 6c. Asking why, and looking at it

```cpp
gi::ExplainSettings ask;
ask.frame = frame;
const gi::IlluminationQuery why = system.explain(point, normal, surface, ask);
// why.cause: Lit, Occluded, Unconverged, MissingProbes, LowConfidence or Invalidated
// why.probes[i]: weight = falloff * facing * visibility, age, error, leak_suspect
const gi::GiProfile cost = system.profile();   // after the frame's queries, before the next update
```

`explain` answers with the resolve's own function, so the answer is bit for bit what the frame was
lit with. A **leak suspect** is a probe that contributes although the distance field blocks the line
from it to the point: the probe-visibility view paints those surfaces red and the probes as red
discs. `profile()` gives the six stage costs, rays per tier, hit rates and the frame's
invalidations attributed to the change that filed them.

The nineteen debug views (`gi/debug_views.h`) are `render::DebugViewMode`s of the same names
(`GiTier`, `GiProbeVisibility`, `GiSurfaceCache`, ...). Draw one with `gi::render_gi_debug_view` for
the frame's camera and surface buffer, upload it with `lightmaps::GiDebugOverlayTexture`, and
`lightmaps::write_gi_debug_view(mode, slot, view)` makes the forward pass draw it where it is
opaque. `LightmapDensity` and `LightmapCharts` are drawn by the frame itself.

![The nineteen GI debug views](../design/images/gi-debug-views.png)

**On the device, in the frame.** `gi_gpu::GpuGiDebugView` draws the same nineteen views inside the
frame's `ScreenSpaceGi` stage — `GpuGiFrame::set_debug_view` asks for one — from the device frame's
own state: the resolve's surfaces (the prepass the frame drew), its published targets and its
reprojection history, the probe set, the cards and their lookup, and the field. Each is a
transcription of its function in `gi/debug_views.cpp` (`gi_debug_views_common.slang` names which),
and `smoke.gi_gpu_live` (c) holds every one to the host's view over the frame's surfaces:

| | The device draws | Held to the host's view |
|---|---|---|
| the four probe views, the clipmap, the surface cache, the field slice, reflection probes, screen trace, tier, ray count, the history views, emissive | the host function's datum, from the device copies | word for word (RTX 5060: 0 of 129 600 texels differ) |
| `GiProbeVisibility` | every contributing probe's leak test (the host's report keeps 32) | 1 texel of 129 600 |
| `GiIndirectDiffuse`, `GiIndirectSpecular` | the published targets the forward pass composites | within one 8-bit step: the targets are halves |
| `GiConfidence` | `RadianceCache::sample`'s confidence at the surface | word for word against `sample` |
| `GiCards` | the surface-cache page `CardSnapshot::find` resolves | word for word against the lookup's own rule |
| `GiScreenTrace` | red: the device frame's rays have no screen tier | `screen_trace_texel` with no screen view |

A view's exposure is automatic — the frame's own mean indirect diffuse drawn at a third of white —
unless the caller names one. `GiReflectionProbes` over a system that holds reflection probes is the
one view the device does not draw (`GpuGiDebugView::draws`): their captures are the host's, and the
caller draws it on the processor. A frame with a view costs what the frame costs plus the view's
dispatch: 8 to 15 ms for the whole 480x270 frame in `smoke.gi_gpu_live` (d), where the processor
took 35 to 240 ms for the view alone at 320x180.

## 6d. Tuning it while the world runs

Four calls change the GI of a running world without `configure()`'s full rebuild. They are what a
game's options menu, a level script and the editor all reach — the editor through its document, a
Swift game through ABI 1.9 — so an author sees exactly what a player gets.

```cpp
gi::GiRuntimeSettings settings = system.runtime_settings();
settings.quality = gi::GiQuality::High;        // the global probe quality, a volume's where one reaches
settings.tier = gi::GiTierPreference::Automatic;
settings.hardware_ray_tracing = false;           // exactly a device without ray queries
settings.budget_ms = 2.5F;                       // the allocation the next update holds to
(void)system.set_runtime_settings(settings);

const gi::EmitterRule out{gi::kAnyEmitter, lamp_glass_material, gi::EmitterOverride::Off};
(void)system.set_emitter_rules({&out, 1}, frame); // the lamp is put out

(void)system.set_object_class(cart_id, true, gi::IlluminationClass::Static, frame);
```

| Call | What changes | What it costs |
|---|---|---|
| `set_runtime_settings` | mode, global quality and tier, the hardware tier's profile switch, the budget | a quality or tier change retires every probe (both are decided at placement); the switch keeps its structures; nothing else is touched |
| `set_emitter_rules` | per material, per instance or both: `Automatic`, `CacheOnly`, `Promote`, `Off` — the most specific rule wins | only cells with an emitter are classified again, from the cards they came in with; a promoted light that leaves or arrives invalidates what it lights |
| `set_object_class` | an object's class, or back to the derivation | entering Static or SlowDynamic places its cards and its own field now; leaving takes them out now, keeping the field for the way back |

Untouched, each is the configured system bit for bit (`integration.render_gi_runtime`'s "the
run-time settings start as the configured ones, and applying them is a no-op",
`render_gi_emissive`'s "with no rule, classification under rules is the measurement's"). A lamp put
out and lit again shades the floor under it to the same bits it had (`render_gi_emissive`); an
object made Static at run time resolves every point to the same bits as one added Static
(`render_gi_classes`). A scaled object's cards now carry the area of its scaled surface, so a
scaled lamp's power is the world's — before, a lamp carded over a unit square and placed at a
third of its size emitted over a whole square metre, eleven times its power.

## 6e. From the editor

The Lighting & Lightmaps panel has a global illumination half; every edit is a registered,
undoable `gi.*` command with an MCP tool of the same name ([editor/README.md](../../editor/README.md),
"Global illumination").

| Command | The document | The engine |
|---|---|---|
| `gi.settings.set` | `GiSettings` on the world's one "Global Illumination" node | `set_runtime_settings` |
| `gi.volume.create`, `gi.volume.set` | a node whose transform is the box (translation the centre, scale the size: the move and scale gizmos place and size it) and a `GiVolume` with its `sets` mask | `set_volumes` |
| `gi.object.set-class` | `GiObject` on a placed object (also the Inspector's row) | `set_object_class` |
| `gi.emission.set` | `GiEmission` on the world's one rule node for a material | `set_emitter_rules` |
| `gi.report`, `gi.status` | — | `illumination.report`: settings, tier, convergence, cost per stage, rays, hit rates, every emitter's class and every object's, and `explain()` at a clicked pixel |
| `viewport.view-mode.gi-*` | — | the nineteen GI debug views, drawn by the runtime over its own frame |

The runtime (`samples/05b-editor-window/runtime/gi_runtime.h`) reads the document into the engine's
`cy::editor::IlluminationPreview` (`src/editor_backend`), which rebuilds the GI only when geometry,
materials or lights changed and applies everything else in place through the calls of section 6d
— the sky too: it is the frame's own flat ambient (the studio fill in the Editor view), so the GI
that replaces the ambient lights an open surface as the ambient did.

**The viewport is lit by the device GI.** A world whose "Global Illumination" node admits the
dynamic GI has its Lit view lit by `gi_gpu::GpuGiFrame` over the preview's system: every frame the
runtime advances the system one frame from the camera and the frame resolves it on the device, the
caches' arithmetic on the device too (section 6g). Every GI view the viewport asks for
(`GizmoIntent::debug_view`) is drawn by the device in the same frame. A world without the node is
drawn as it always was, and a GI view over it resolves the GI for the view without compositing it
into the Lit pixels. Where the device cannot run the GI — the device GI has run on Vulkan only — the
views are the processor's, at a quarter of the frame's resolution, as before.
`smoke.editor_authored_frame_vulkan`'s device GI case measures it: the Lit view changes 44 435 of
98 304 pixels, the document's quality set to low reaches the next frames with nothing rebuilt, mode
"none" is the plain frame byte for byte, every view is drawn, a frame takes 4 to 8 ms at 384x256
with the GI 0.15 to 0.3 ms of it on the device — where the processor took 45 ms for the indirect
diffuse view alone at a quarter of the size — and detached, the frame is the frame it was.

![The editor's viewport: without the GI, lit by the device GI, and in four device views](../design/images/editor-gi-live.png)

**A Swift game in the editor's Play tunes the same GI.** The runtime binds ABI 1.9 for Play over
the preview's system — every object by its Play entity, every material by its authored name — so a
project's behaviour places volumes, puts lamps out and reads the light the author sees lit in the
viewport. Stop applies the document's GI again.

![The editor viewport lit and in the GI Tier view](../design/images/editor-gi-viewport.png)

## 6f. From a Swift game

```swift
var settings = try Illumination.settings                 // a graphics options menu
settings.quality = .high
try Illumination.apply(settings)

let crypt = try GIVolume.place(.init(                    // a level's important interior
    bounds: (min: Vec3(x: -8, y: 0, z: -8), max: Vec3(x: 8, y: 4, z: 8)),
    overrides: [.quality(.epic), .updateRate(2)]))
try Illumination.setClass(.fastDynamic, of: cart)        // it moves every frame
try Illumination.setEmission(.off, of: torch)            // the guard put the torch out

let light = try Illumination.irradiance(at: thief.position)   // onUpdate
if light.luminance < 0.05 { hidden = true }                    // act on it in the next step
```

`irradiance(at:normal:)` is `indirect_diffuse` at the point; with no normal, the mean over the six
axis directions — what a body standing there receives. Every call is refused in `onFixedUpdate`
(illumination is presentation, so the simulation never depends on it), which is why an AI reads the
light in its frame update and acts on it in the next fixed step. `integration.swift_illumination`
runs a real Swift module against the GI suites' room: the light at a guard's spot is the system's
own six-direction mean to the micro-unit, and putting the lamp out by its material's name halves it.
The C reference is [src/abi/README.md](../../src/abi/README.md), "What ABI 1.9 adds".

## 6g. Turn on GI in your game

The GI an author tunes in the editor and a player sees in the game is one system: an
`IlluminationSystem` the host runs, lit on the device by one frame feature,
`gi_gpu::GpuGiFrame`, and tuned by the game through ABI 1.9. Three steps in the host, then the game
is Swift. `samples/13-rts-api` does all of it (`host/illumination.*`, `host/render.*`,
`game/Lighting.swift`).

**1. Hand the level to the GI.** Configure a system and give it the level's surfaces — cards over
each mesh and its distance grid placed by its transform (`gi::append_mesh_cards`,
`gi::MeshDistanceField`), static geometry as a cell, what moves as an object (`add_object`, then
`move_object` each frame) — and the sun **in the bake's units, lux over pi** (section 9's
pitfall).

**2. Let the game tune it, and run it once a frame.** Bind the adapter on the ABI host, naming the
materials and the entities a script may address, and run the system's update after the game's
frame, from where the camera looks — presentation, which a fixed step never reads. Size the
budget's ladders to the level (below):

```cpp
cy::game_backend::IlluminationAdapter adapter(allocator, system);
(void)adapter.add_material("beacon", beacon_material_id);       // what Swift names it by
(void)adapter.bind_object(crate_entity, crate_object_id, crate_instance_id);
cy::game_backend::bind_illumination(host, &adapter);
// each frame:
adapter.begin_frame(frame);                     // the game's changes are stamped with it
runtime.frame_update(dt);                       // the Swift game's onUpdate
gi::FrameContext context{};
context.camera = focus;            // where the camera looks; an RTS's is the ground point
context.lights = sun; context.frame = frame;
(void)system.update(context);                   // the host decides; the device shades and gathers
```

**3. Light the frame with it.** Create the feature once over the system, and attach it to the
assembled frame in the frame's two hooks:

```cpp
gi_gpu::GpuGiFrame gi;
(void)gi.create(allocator, device, system, {width, height});    // outside a device frame
description.global_illumination = true;     // or assembly.set_global_illumination(on), per frame
// before assemble:
gi_gpu::GpuGiFrameTargets targets;
(void)gi.prepare(graph, {eye, world_to_clip, projection, relative_to_view}, frame, targets);
view.screen_space_gi = targets.diffuse;
view.screen_space_gi_specular = targets.specular;
sinks.screen_space_gi = gi.stage();
// once the upload exists:
gi.set_jitter(jitter);
gi.write_control(diffuse_slot, specular_slot, upload.view.gi_control);   // bind the two views there
```

`create` mirrors the system's acceleration structures on the device where it has ray queries and
installs the device's card shading and probe gather through the system's own seams
(`GpuGiFrameDescription::device_caches`, on by default): the host's update is the decisions alone,
and the arithmetic runs in the frame, before the resolve that reads it. `set_debug_view` draws any
of the nineteen views in the same frame (section 6c). A mode that admits no dynamic bounce
(`composites()` false) composites nothing, and a frame that does not attach the feature is the
frame it always was.

**4. The game.** Everything else is the game's, from Swift (section 6f and the Swift guide's
"Turn on GI in your game"):

```swift
// onCreate: the base converges first and best.
let base = try GIVolume.place(.init(bounds: (min: Vec3(x: 3, y: 0, z: 22), max: Vec3(x: 16, y: 5, z: 31)),
                                    overrides: [.quality(.epic), .updateRate(2)], priority: 1))
// onUpdate: the beacon out, and on again.
try Illumination.setEmission(.off, ofMaterial: "beacon")
let light = try Illumination.irradiance(at: guardSpot, normal: facingBeacon)
try Illumination.setEmission(.automatic, ofMaterial: "beacon")
```

**In the editor** a Swift project needs none of the host's steps: give the world a "Global
Illumination" node (`gi.settings.set`) and the viewport is lit by the device GI, and in Play the
project's behaviours reach that same GI through ABI 1.9 (section 6e).

**What it costs** (`smoke.rts_api_gi`, RTX 5060, Vulkan, 480x270, `samples/13-rts-api`'s 32 m
field from the game's camera): the game's frame 1.3 ms of which the GI's host update is 1.1 ms;
the drawn frame 5.1 ms with the GI's device passes 1.0 ms of it (the resolve, the card shading
and the probe gather; 0.76, 0.55 and 0.15 ms in `smoke.gi_gpu_live`'s scene); 6.3 ms a frame
together, where the drawn frame without the GI is 3.9 ms. Two choices make it that: the caches'
arithmetic on the device (with them on the host the update is 3.7 ms), and **ladders sized to the
level** — `system.budget().declare(GiLever::ProbeUpdates, ...)` and `SurfaceCacheRate` at a quarter
of the engine's defaults, which are for a world rather than a 32 m field (at the
defaults the host update was 22 ms a frame). An RTS camera hangs 25 m over the field, so the host
runs the GI from the ground point it looks at (`FrameContext::camera`): the clipmaps follow what
the player sees rather than the air the camera hangs in.

![samples/13-rts-api: without the GI, with it, the beacon put out from Swift and lit again](../design/images/rts-api-gi.png)

**An emitter a frame should show lights through the cache.** A *promoted* emitter's light is
direct light (section 5), and the forward frame's own lights do not carry promoted emitters yet
(section 10): the frame shows a promoted lamp only in the bounce. The RTS beacon is larger than
`promotion_max_area_m2`, so its light reaches its surroundings through the surface cache's gather —
the light the frame's GI composites — and the frame shows the game putting it out (the frame round
its foot falls from 169 to 148 and rises again to 170; without the GI it reads 197 throughout).

## 7. Measuring it: against the oracle, and against the engines it competes with

"Better than Lumen or Unity's adaptive probe volumes" is only worth saying as numbers a suite fails on.
The claims and where each is measured:

| Claim | Measured by | Today (RTX 5060) |
|---|---|---|
| The device computes what the host computes | `render.gi_gpu`, `render.gi_gpu_tracing`, `render.gi_gpu_resolve` | card radiance 3.7e-8, probe payloads 5.7e-8, the query 1.2e-7; the per-pixel resolve, denoiser and upsample about 1e-7 mean relative, both tiers, the history exact |
| Hardware and software tiers agree within a stated bound | `render.gi_gpu_tracing`, `integration.render_gi_fallback` | 0.0209 against a 0.10 tolerance |
| Losing ray queries is not a visible event | `render.gi_gpu_tracing` | the switch alone moves the image 0.074; an ordinary frame 0.142 |
| No light through a wall | `integration.render_gi_screen`; `smoke.gi_gpu_probe_quality`; `integration.render_gi_screen_probes` | a lit room and a sealed one behind a 10 cm wall: at 40x24, the per-pixel gather's sealed room mean 2.6e-5 of the lit room's, its worst pixel 7.6e-3; at 1920x1080 the screen probes' (the default) 0 and 0 through both tiers, the per-pixel gather's 2.5e-5 and 4.1e-2 (software) — the path tracer says zero |
| Close to the path tracer | `smoke.gi_gpu_probe_quality`; `integration.render_gi_screen`; `just capture-beauty-gi`; `just capture-gi-interior` | at 1920x1080 over 220 points, the screen probes 0.071 (software) / 0.064 (hardware) relative (0.076 / 0.066 through the shared denoiser), the per-pixel gather 0.078 / 0.065; at 40x24 the per-pixel gather 0.112 (the probes alone 0.436), 0.148 on a half-resolution grid; the beauty shot 0.048 over 288 pixels against the exact triangles through the screen probes (0.127 per pixel), where the flat sky term it replaces is 0.384; the sunlit room and the Colonnade, every traced pixel, in section 7b |
| Fast convergence after a change | `smoke.gi_gpu_probe_quality`; `smoke.render_gi_screen_convergence` | a light moved 4.9 m, within 10 % of the new light's converged frame: at 1920x1080 the screen probes in 9 (software) / 10 (hardware) frames (14 / 13 through the shared denoiser), the per-pixel gather in 9 / 11; at 40x24 the per-pixel gather in 11 (bound 14) |
| Temporally stable | `smoke.gi_gpu_probe_quality`; `integration.render_gi_screen` | a converged still view at 1920x1080 moves 0.0016 / 0.0011 a frame through the screen probes (0.0019 / 0.0017 through the shared denoiser), 0.0023 / 0.0017 per pixel; at 40x24 per pixel 0.0034, where the one-ray signal under it moves 0.451 |
| Off is the frame it was | `render.gi_gpu_frame`, `just capture-beauty-gi` | byte for byte, the stage attached with its words at none, through either gather; the beauty shot without `--gi on` |
| A leak is named, not guessed at | `integration.render_gi_pipeline` (the visibility term), `integration.render_gi_volume`, `integration.render_gi_diagnostics` (leak suspects) | the divider's face takes 0.075 of its probe weight from probes the field says cannot see it (0.135 before the per-axis visibility test); the rest is doorway-edge probes |
| A moving body darkens what it stands on, invalidating nothing | `integration.render_gi_classes` | a FastDynamic character's proxy within 0.044 of the open light of the path tracer tracing a triangle sphere; a projectile files 0 invalidations, a vehicle 8 in 32 frames |
| Leaves transmit, fog is lit by the cache | `integration.render_gi_media`, `render.volumetric_fog` (g) | thin transmission 0.123 and the medium's grid 0.125 (mean relative) from the path tracer; the device fog within 2.3e-5 of the host march |
| Emitters lit without fireflies, within a stated error | `integration.render_gi_emissive` | a promoted 30 cm lamp 0.5 % from the path tracer with a per-receiver ratio variance of 0.027 (the reference's noise); left to the cache, 4.6; a promoted panel 2.0 % |
| A region gets the quality it is marked for, within one budget | `integration.render_gi_volumes` | 3.9x the probe updates and 2x the rays in the marked interior, the frame's probe budget unchanged; no volume reaching a point is bit for bit no volume at all |
| Tuning it live changes only what it says | `integration.render_gi_runtime`, `integration.render_gi_emissive`, `integration.render_gi_classes` | untouched settings, no rule and a class set at run time are each the configured system bit for bit; a lamp put out and relit shades its floor to the same bits; Epic quality is 4x the rays of every probe placed after it |
| A game reads the light where a body stands | `integration.game_backend_illumination`, `integration.swift_illumination` | the sample is `indirect_diffuse`'s own answer bit for bit (the six-direction mean for a body); a lamp put out from a script takes the luminance under it from 2.150 to 1.038 in the adapter and from 2.149 to 1.032 through Swift |
| Every GI view in the editor's viewport, and Lit as before | `smoke.editor_authored_frame_vulkan`, `integration.editor_window_illumination` | 11 136 of 11 200 opaque view texels change the frame, every one of 13 376 clear texels keeps it, and back on Lit the frame is byte for byte the frame before |
| The views drawn by the device are the host's | `smoke.gi_gpu_live` (c) | seventeen of nineteen word for word over 129 600 texels, `GiProbeVisibility` within one texel, the indirect diffuse and specular within one 8-bit step (the targets are halves) |
| Live in the editor and in a game, at interactive rates | `smoke.editor_authored_frame_vulkan`'s device GI case, `smoke.gi_gpu_live` (a), (b), (d), `smoke.rts_api_gi` | the editor's Lit view lit by the device GI, a frame 4 to 8 ms at 384x256 with any view; a document edit or a game's ABI 1.9 call in the next frames, nothing rebuilt; the RTS frame 6.3 ms at 480x270 with the GI 1.0 ms on the device and 1.1 ms on the host; off, the frame it was byte for byte |
| Explicit cost | `smoke.gi_gpu_probe_quality`'s cost cases; `render.gi_gpu_tracing`'s and `render.gi_gpu_resolve`'s; the beauty shot's report | gather at budget 0.078 ms (software) / 0.043 ms (hardware). The GI at 1920x1080, every stage: the screen probes (16 px) 1.99 ms (hardware) / 2.51 ms (software) — 4.75 / 5.27 through the shared denoiser — and 1.19 / 1.65 ms on the half-resolution grid; every 8 px 3.51 / 5.15 ms; the per-pixel gather 10.6 / 13.9 ms, 2.86 ms on the half grid, 1.96 ms with the denoiser lever too. For a 3840x2160 output the probes are 7.7 / 9.7 ms native (per pixel 42.4) and 3.5 / 4.4 ms at the upscaler's 1440p (per pixel 18.9). The scenes of section 7b (the beauty sample, every stage, the fastest of 240 frames at full clocks): the sunlit room 3.49 ms (hardware) / 4.61 ms (software), 2.40 ms on the half grid; the workshop hall 3.78 / 5.40 ms, 2.63 on the half grid; the Colonnade 2.73 / 5.68 ms, 1.73 / 3.88 ms on the half grid (per pixel, 12.3 ms). Stage 5 holds a 960x540 frame of the screen probes to its allocation by dropping from 0.68 ms to 0.35 ms in the declared lever order |

**The path tracer as the reference.** `PathTracer` (`gi/bake.h`) is the same tracer the lightmap bake uses, over the same GI scene. For a
reference over cards turn on `set_emission_footprints(true)`: a hit then takes the emission of the
card whose square holds it, rather than of the nearest card — without it an emitter smaller than its
neighbours' spacing loses its corners and the reference reads 5.6% low (measured, and the reason the
mode exists). Compare in the units the caches use: `irradiance()` returns the mean cosine-weighted
incoming radiance, which is a card's `direct + accumulated / albedo`.

Report the energy over a set of receivers and the per-receiver error separately: the first averages
the reference's noise out, the second includes it. `render_gi_emissive`'s `Agreement` does both,
and prints them.

### How this compares with Unreal's Lumen and Unity's GI

No head-to-head run exists: neither engine runs in this repository, and no one scene has been
measured in all three. What can be stated is CyberGI's MEASURED number on each axis of the program
goal beside what the other two DOCUMENT about the same axis; where a vendor publishes no number, the
cell says so rather than inventing one. Every CyberGI number below is a test that fails when it
regresses.

| Axis | CyberGI (measured, RTX 5060 where a device is involved) | Unreal Engine 5, Lumen (documented) | Unity 6, Adaptive Probe Volumes and Enlighten (documented) |
|---|---|---|---|
| Error against a path-traced reference | at 1920x1080 in the GI test room (220 points), the screen probes 0.071 (software) / 0.064 (hardware) mean relative, the per-pixel gather 0.078 / 0.065; 0.112 per pixel at 40x24, 0.048 on the beauty shot against the exact triangles; held below 0.16, the probes within 0.02 of the per-pixel gather | no error figure against a reference is published | APV stores what the baker path-traced, so a probe's value is a bake's; the error of sampling probes between them is not published |
| Convergence after a light moves | within 10 % of the new light's converged frame after a 4.9 m move: the screen probes in 9 (software) / 10 (hardware) frames at 1080p, the per-pixel gather in 9 / 11 (11 at 40x24); held at 14 | accumulated over frames; update speed is a set of tuning variables, no frame count is published | a probe volume is re-baked or blended between pre-baked lighting scenarios; Enlighten updates a moved light's bounce over several frames over static geometry only |
| Light through a thin wall | a sealed room behind a 10 cm wall at 1080p: through the screen probes none at all, both tiers; through the per-pixel gather a mean 2.5e-5 of the lit room's light and a worst pixel 4.1e-2 (software); at 40x24 per pixel 2.6e-5 and 7.6e-3 (held below 1e-3 and 2e-2) | the documentation advises walls of at least 10 cm for the software tracer to avoid leaking | leaking between probes is a documented APV artefact, reduced by its Virtual Offset and leak-reduction settings; no figure |
| Temporal stability | a converged still view at 1080p moves 0.0016 (software) / 0.0011 (hardware) a frame through the screen probes, 0.0023 / 0.0017 per pixel; 0.0034 per pixel at 40x24, where the one-ray signal under it moves 0.451 (held below 0.01) | no figure | baked probes do not move at all, and do not respond to a light that does |
| Emissive lighting | a promoted 30 cm lamp within 0.5 % of the path tracer with a per-receiver ratio variance of 0.027; left to the cache the same lamp is 4.6 — promotion is the measurement, or an author's rule | emissive surfaces light through the GI's gather; the documentation recommends real lights for small bright emitters, which are noisy | emissive surfaces contribute to the bake; realtime emission through Enlighten |
| Hardware ray tracing with a bounded software fallback | the tiers agree to 0.0209 (held at 0.10); switching moves the image 0.074 where an ordinary frame moves 0.142 | hardware and software tracing are both offered; the difference between them is not bounded in the documentation | runtime probe GI does not trace rays; ray-traced GI is a separate HDRP feature |
| Cost at 1080p | the whole GI through the screen probes 1.99 ms (hardware) / 2.51 ms (software) at authored quality and full resolution in the GI test room, 3.49 / 3.78 ms (hardware) in the furnished sunlit room and workshop hall (section 7b); 1.19 / 1.65 ms on the half-resolution grid; per pixel 10.6 / 13.9 ms, 2.86 ms on the half grid; a 960x540 frame held to its allocation from 0.68 ms to 0.35 ms | budgets are documented per scalability level for consoles; no RTX 5060 figure | a probe lookup per pixel at runtime; the bake's cost is offline |
| Control from tools and code | every setting, volume, class and emitter rule from the editor (undoable, MCP) and from a Swift game (ABI 1.9), seen in the next frames of the device GI that lights the viewport and the game; all nineteen debug views drawn by the device in the viewport; `explain()` at a clicked point | post-process volume settings, console variables, visualization modes | the Lighting window, probe volume baking sets and lighting scenarios, a scripting API |

What the table supports, and only that: on every axis where a number exists, CyberGI's is stated,
held by a test and reproducible from this repository; on error against a reference, leaking at
10 cm, convergence and stability neither vendor publishes a number to compare it with. The screen
probes, Lumen's own technique, denoised in probe space, cost CyberGI a quarter of what its
per-pixel gather did at 1080p, with less error, no leak, a stiller view and a moved light settled
as fast. What it does not support: that CyberGI is cheaper than either engine — at full resolution
its default is 2.0 ms through the hardware tier in the test room and 3.5 to 3.8 ms in the furnished
interiors of section 7b (4.6 to 5.4 ms through the software tier), a fifth of a 60 fps frame; that it was ever measured
beside either engine on one scene; or anything about consoles, Windows or Apple devices, where the
hardware tier refuses (section 10). The editor's GI views are drawn by the device in the viewport's
own frame on Vulkan, as Lumen's visualization modes are drawn by its GPU frame (section 6c); on a
device that cannot run the GI they are the processor's — about 35 ms for the probe view at 320x180
and 240 ms for indirect diffuse (`smoke.editor_gi_cost`, GCC 13).

### 7b. Beside the path tracer, as a picture: a sunlit room, a workshop hall and the Colonnade

The numbers above are measured at chosen points; `just capture-gi-interior` photographs one view
three ways and maps the difference at every fourth pixel each way. The view without the GI shades
every surface with the frame's flat sky term; CyberGI is the engine's default — the screen probes
every 16 pixels, 32 rays a probe, every probe traced every frame, their probe-space denoiser, the
full-resolution grid, through ray queries; the path tracer's frame is the same frame with the GI's
indirect diffuse replaced by `gi::PathTracer`'s (the lightmap bake's, over the exact triangles,
three bounces, an emitter emitting over its cards' squares), traced at every fourth pixel and
upsampled against the frame's depths and normals, so the sun, its shadow, the GI's specular and the
sky are the frame's own in all three. The error map is |CyberGI - path tracer| over the path
tracer's luminance, from black through red to pale yellow at 50 % and above. Each caption carries
its own figure, `compare_against_reference`'s over every traced pixel: the flat sky term's under
the frame without the GI, the GI's and its GPU time under CyberGI, and under the path tracer its
own noise — read from the difference of two independent halves of each pixel's paths, the part of
the error the GI is not answerable for. The GPU time is every stage of the GI on the RTX 5060 at
1920x1080, the fastest of 240 frames of the same view drawn after the still with the host's cache
held, so the device runs at the clocks a game's frames keep it at (section 9).

**A room where indirect light is most of the picture** (`content/beauty/interior.cyshot`): a closed
room, a red wall on the left and a green one on the right, white plaster elsewhere, and the sun 35
degrees up through one window in the far wall. Everything out of the sun's patch on the floor is
lit by what that patch and the sky in the window throw back.

![The sunlit room: without the GI, with CyberGI, through the path tracer, and the error](../design/images/gi-interior-comparison.png)

Measured by `just capture-gi-interior` on the RTX 5060, Vulkan, 0 validation errors: **CyberGI
0.091 relative to the path tracer over 125 024 traced pixels, where the path tracer's own noise
(2048 paths, three bounces) is 0.056; 3.49 ms on the GPU** — the probes' rays 1.48, the probe-space
filter 0.01, the integration 0.74, the denoiser 0.58, and the surfaces' unpacking and
reprojection 0.67 — through ray queries at full resolution; 4.61 ms through the software tier, 2.40
on the half grid. It was 0.193 and 6.63 ms (rays 3.84, integration 1.13); the code before this
change, timed at full clocks, 6.24 ms (software 9.28). The error is to its reference's noise as
the Colonnade's is to its own — 1.6 times — and the noise is part of it: a reference of 2048 paths
differs from the converged image by about its noise, so an exact GI would read about that much.
The flat sky term the GI replaces is 7.85: indoors it lights every surface as if the room had no ceiling. Its sunlit patch is where its window
and sun put it (`tools/docs/check_sun_patch.py`: 233 of 255 in the patch, 24 around it).

*Why it was dark.* The surfaces lit mostly from the sun's patch — the far wall at 0.68 of the path
tracer's light, the green wall and the ceiling at 0.77 — read the patch's cards, and the patch's
cards were short of it: a card was shadow-tested at its centre, so each was all lit or all dark,
and in this room the cards whose centres fell in the 2.3 m^2 of sunlight on the floor stood for
1.5 m^2. A card's direct light is now the mean over its square (section 9).
`integration.render_gi_pipeline` rebuilds the room from its boxes, cards it as the sample does and
holds it to that: the floor's cards carry 1.049 of the window's sunlight (0.668 before), and their
one bounce onto 32 points of the ceiling and the far wall is 1.008 of the path tracer's, 0.068
relative error (0.726 and 0.274 before).

*What is left* is mostly at contact: the error map's brightest marks are the floor's strip along
the foot of the far wall, the ceiling's corners and the room's vertical edges, where CyberGI is
brighter than the path tracer — the occlusion within a few centimetres of a corner, below the
probes' spacing (section 10).

**A game's interior** (`content/beauty/hall.cyshot`, *The Workshop Hall*): two tall windows in the
left wall with the afternoon sun through both, a blue wall, an ochre far wall, a timber bench under
a hanging lamp, two columns, a stone block in the first window's light, a plaster orb and painted
crates — the weathered-stone and gravel graphs and their maps, coloured per material. The lamp's
underside glows (an emissive decal, `--decals on`); the frame has no light for it, so without the
GI it lights nothing, and with the GI its light reaches the bench and the wall through the screen
probes' rays as the path tracer's does.

![The workshop hall: without the GI, with CyberGI, through the path tracer, and the error](../design/images/gi-hall-comparison.png)

**CyberGI 0.148 relative to the path tracer over 129 600 traced pixels, the path tracer's own noise
0.088; 3.78 ms on the GPU** (the probes' rays 1.68, the integration 0.82, the denoiser 0.58),
5.40 ms through the software tier and 2.63 on the half grid, 14 301 cards, 4 of them the lamp's. The flat sky
term is 6.72. The lamp's light on the bench and on the blue wall under it is the path tracer's;
the error map's brightest marks are again the contact edges — the floor along the walls' feet, the
corners of the window bays — and, more than in the sunlit room, the floor about the bench and the
block, where the reference's noise is highest too. The lamp lies on the GI's half-metre card
lattice: across it, the reference's footprint mode lit only 78 % of it (section 9) and the same
hall read 0.214.

And in motion — the camera swinging 30 degrees about the hall's target and back over four seconds,
the GI at its default every frame, reprojecting its probes and its history through the motion
(`just capture-gi-hall-video`): [docs/design/videos/gi-hall-orbit.mp4](../design/videos/gi-hall-orbit.mp4).

**The Colonnade** (`content/beauty/shot.cyshot`), outdoors under a low sun, where the sky does most
of the indirect work:

![The Colonnade: without the GI, with CyberGI, through the path tracer, and the error](../design/images/gi-colonnade-comparison.png)

**CyberGI 0.023 relative over 86 427 traced pixels, the path tracer's own noise 0.014; 2.73 ms on
the GPU** (the probes' rays 0.90, the integration 0.66, the denoiser 0.53), 12 782 cards; it was
0.022 and 4.49 ms, its denoiser's 0.93 ms then including the reference's copies (section 9), and
4.09 ms timed at full clocks. The flat sky term is 0.351. The error map's brightest marks are thin lines where
the blocks and the plinths meet the gravel.

## 8. Running and changing it

```sh
just build-engine
just test-integration -R render_gi            # the host system and the oracle, no device
CY_RENDER_UPDATE_GOLDEN=1 build/dev/cy_test_integration_render_gi_debug_views   # rewrite the view goldens
just test-unit -R render_raytracing           # the service and its mirror, no device
just test-render -R gi_gpu                    # the device suites; skipped where there is no device
just test-integration -R render_gi_screen_probes   # the screen probes' properties, on the host
just test-smoke -R gi_screen_probes           # their quality at 128x72 on the host
just test-smoke -R gi_gpu_probe_quality       # both gathers' quality and cost at 1080p and 4K, on the device
just test-smoke -R gi_gpu_live                # GpuGiFrame on, off and tuned; the device views against the host's
just test-smoke -R editor_authored_frame      # the editor's viewport lit by the device GI, every view on it
just test-smoke -R rts_api_gi                 # a Swift game's frame lit by it, and what it costs
just run-sample rts-api --render <dir>        # the RTS drawn with the device GI (--gi off: without)
just capture-gi-interior                      # a sunlit room, a hall and the Colonnade: off, CyberGI, path traced
just capture-gi-hall-video                    # the hall lit by CyberGI as the camera swings round it
python3 src/rendering/gi_gpu/shaders/regenerate.py --slangc build/dev/Development/bin/slangc
just build-shaders --strict src samples       # every entry point for SPIR-V, MSL and DXIL
just quality-shader-headers                   # the committed headers are what the sources compile to
```

A device shader is a **transcription**: each function names the host function it transcribes and
writes it in the same order, because a reordering that is equal in exact arithmetic is a test failure
in float. Change the host function and its transcription together, regenerate the headers, and run
both suites. The [Slang guide](slang.md) has the shader rules.

## 9. Pitfalls this code has already met

- **A signed remainder is not a modulo on every driver.** The RTX 5060 answered `-4 % 9` with 0 when
  the divisor was read at run time. Toroidal addressing (`giWrap`) is unsigned arithmetic for that
  reason; a power-of-two window hid the defect for a whole stage.
- **Host-written device buffers belong in device-local memory.** A march reads its scene per step;
  from system memory a 1080p screen batch took 44 ms instead of about one. Use
  `MemoryUse::HostVisibleDeviceLocal`, which falls back where the device has no resizable BAR.
- **Import a shared buffer once per graph.** Two `import_buffer` calls of one handle are two
  resources and the graph derives no barrier between them; `RenderGraph::import_buffer_once` is the
  call when another module's passes touch the same buffer.
- **One Vulkan device at a time in a process.** The backend loads entry points through volk, which
  holds one device's table; a suite that compares a device with and without ray queries creates them
  one after the other.
- **Exact ties are a property of the scene.** A dyadic scene puts hits exactly on a card's lookup
  sphere or between two cards; the lookup's tie rule (`card_lookup_prefers`) and an off-lattice
  lookup radius keep a comparison about the transcription rather than about the last bit.

- **A snapshot of a cache nothing has shaded is empty.** `CardSnapshot` skips invalid pages, so the
  first card update gathers nothing; emission reaches a neighbour at the second.
- **`scene().ingest_cell` does not classify emitters.** It still lights every emitter through the
  cache. `IlluminationSystem::ingest_cell` classifies, and `evict_cell` releases what it allocated.
- **A volume that only sets intensity or far field re-places nothing** — those are per sample. One
  that sets density, rate, quality or tier re-places its region's probes, which then converge again.
- **Compare a struct's floats as bits field by field, not with `memcmp`**: `GiRegionSettings` has
  padding.
- **`HostVisibleDeviceLocal` is not always device-local.** The resolve's surface buffer, 72 bytes a
  pixel at 1080p, landed in system memory when the small BAR heap was full and the frame's timing
  spiked to tens of milliseconds; it is device-local now, written through an upload copy the graph
  orders.
- **A stochastic sample must not be weighted by what it met.** See section 4: the final-gather ray's
  weight is constant and its confidence decides how much of its value a stand-in supplies.
- **A brick's last voxel needs its far corner.** The field stored four samples per brick axis, so the
  last voxel of every brick was flat and a 10 cm wall at a brick face vanished from one side; bricks
  store five now (`kBrickSampleEdge`), and `integration.render_gi_pipeline` keeps the wall.
- **The GI's lights are in the bake's units.** A card is shaded as `albedo * direct`, so a light in
  lux makes every bounce pi times the frame's `albedo / pi * E`. `lightmap_bake` divides its lights by
  pi for that reason (bake.h) and so does the beauty shot; a caller that hands the system lux is
  lighting its bounce pi times too bright.
- **Cards must cover what a ray can reach.** A hit no card covers is answered by the cache's stand-in
  at best. The beauty shot first carded only the gravel the camera sees, and a column's shadowed face
  — which sees the ground out to the horizon — resolved a third of its bounce.
- **A shadow map the RHI rasterised has the light's +Y in row 0.** Clip +Y is the top of every
  framebuffer (Vulkan's through its flipped viewport), so a lookup is `v = 1/2 - ndc.y / 2`, as
  `cy/frame.slang` reads it. The beauty sample read `ndc.y / 2 + 1/2` and took every texel mirrored
  across the map's centre line: the Colonnade's pillars cast no shadow on the gravel and nobody
  noticed, because it looked like a sunny courtyard; the sunlit room's side wall, lit by the sun
  through its ceiling, gave it away. `tools/docs/check_sun_patch.py` holds the room's window patch
  where the window and the sun put it.
- **A pass declares every image it binds.** The probes' temporal pass binds the frame's two targets
  on every grid; on the half grid it first declared them only on the publish after it, and the
  descriptor named an image in a layout no barrier had put it in.
- **A card is one value over its square, so shade it as one.** A card answers every hit on its half
  metre of surface, and its direct light was a shadow test at its centre: all lit or all dark. A
  window's sunlit patch a few cards across kept or lost whole cards by where their centres fell —
  the sunlit room's floor carried 1.5 m^2 of lit cards for 2.3 m^2 of sunlight, and everything the
  patch lit read two thirds of its light. `shaded_card_direct` takes each light's share of the
  card's square (2x2 shadow tests, 4x4 where they disagree); `integration.render_gi_pipeline`'s
  window-light cases hold the floor's cards to the window's sunlight and their bounce to the path
  tracer.
- **A probe's trace waits for its slowest lane.** Halving the screen probes' rays saved 13 % of the
  sunlit room's trace, because the time went into the card lookup each hit resolves through — 27
  bucket reads and as many scattered lists a hit — and a 32-ray probe in a 64-lane workgroup left a
  second wave waiting at every barrier. The lookup reads one cell's list now
  (`gi::CardNeighbourGrid`) and a probe of 32 rays a 32-lane workgroup; both change no answer.
  Take a pass apart by removing one thing at a time before believing what costs.
- **A shadow ray to the sun need not march past the field.** Beyond the box every brick lies in
  (`DistanceField::reach_bounds`) each step reads a far value and finds nothing, yet a ray to a
  target 1000 m away kept stepping until its step limit. `SoftwareTracer::occluded` ends the march
  at that box — the same answer, held by `integration.render_gi_pipeline` over 600 rays — which
  paid for most of the card's footprint shadowing on the host: `render.gi_gpu_frame`'s case (b)
  had gone from 4.85 to 5.02 s of CPU against its 5 s budget, and is 4.88 s.
- **Do not read back the frames you time.** With the path tracer's reference on, every frame copied
  its signals back for the host — 0.4 ms inside the denoiser's timestamps at 1080p — and the
  captions quoted that. `GpuScreenResolve::set_readback_frames` turns the copies off; the beauty
  sample copies only its still.
- **A light frame times the device's power state, not the GI.** The beauty sample's frames are a
  few milliseconds of GPU work behind 20 to 30 ms of the host's cache update, and the driver left
  the RTX 5060, run to run, at half its memory clock (P3, 7 GHz) or at its full one (P0, 14 GHz):
  the sunlit room timed 4.81 or 3.49 ms with the same binary, every bandwidth-bound stage up to
  twice as long in the first, and 240 more frames did not settle it. The sample now times the GI
  over 240 frames after the still with the host's cache held — the view is still and converged, so
  an update changes nothing — and the frames, issued back to back as a game's are, timed at full
  clocks in every run. `smoke.gi_gpu_probe_quality` drives the resolve back to back and always did.
- **A reference over cards emits over each card's square.** The path tracer's footprint mode — the
  right one for emitters smaller than the card spacing — gives a card cut by an emitter's edge a
  square smaller than the strip it stands for, and the hall's lamp first lay across the card lattice:
  the reference lit only 78 % of it, and the GI read 7 % brighter than the path tracer over the
  whole hall (0.214). On the lattice the lamp is whole cards and the hall reads 0.148.
- **Fit the GI's sky to the frame's.** `SkyTerm` is a gradient with no sun aureole, and a low sun's
  aureole is most of a clear sky's light: fitted to the atmosphere's zenith and horizon alone, the
  beauty shot's sky lit an upward face with a third of the frame's sky term. Scale it so an upward
  face receives the frame's mean sky radiance (`samples/12-beauty/gi.cpp`'s `fit_sky`).

- **The published targets are halves.** `read_back_published` gives the float the publish computed;
  the `Rgba16Sfloat` texture the frame samples holds it narrowed by the store, which Vulkan lets
  round to nearest or toward zero — the RTX 5060 truncates. A view drawn from the texture is held to
  the float within one 8-bit step, not to the bit.
- **The GI replaces the ambient, so its sky must be the ambient.** Composited, the GI's indirect
  diffuse stands where the frame's flat ambient stood; a sky dome in other units than the ambient
  lights every open surface differently from the frame before. The editor sets the GI's sky to the
  frame's own ambient (the studio fill included); a game sets it once in its units.
- **One texture, one import.** The debug view's texture is written by the stage and read by the
  opaque pass; imported twice, the graph sees two resources and derives no barrier. The host imports
  it once (`GpuGiFrame::prepare` does), and the stage writes that resource.
- **A probe record carries its error.** The device views draw `GiProbeError` from the record's last
  word, which the host upload and the device gather both write; it was zero before, and a device
  view read every probe as settled.

## 10. What is not built yet

- **The screen tier in the screen probes' rays.** The probes trace the world tiers, as the per-pixel
  gather does: the resolve has no previous frame's lit colour to march against, which the device
  screen tier (`GpuTierTracer`) reads.
- **The denoiser and the temporal anti-aliasing share nothing.** The frame's TAA runs on the
  composited colour, after the GI's targets are sampled, and its history clamp would reject the
  GI's frame-to-frame noise rather than average it; what it could replace is now 0.6 ms.
- **Contact edges are too bright.** What is left of the furnished rooms' error (section 7b) is
  mostly where two surfaces meet: the floor's strip along the foot of a wall, a ceiling's corners,
  a room's vertical edges and the floor about an object standing on it, where CyberGI is brighter
  than the path tracer — the occlusion within a few centimetres of the corner, below the probes'
  16-pixel spacing and their origin's lift. The per-pixel gather (0.251 in the sunlit room before
  this rung) is no closer. A short screen-space occlusion term applied to the near field, as Lumen's
  short-range ambient occlusion is, would be the next step; nothing measures one yet.
- **A small bright emitter in the cache.** The hall's lamp stays in the surface cache because the
  frame does not read promoted lights (below), and a half-metre lamp is found by few of a probe's 32
  rays: it lights the bench and the wall under it without fireflies only because it is long — two
  metres of panel. A smaller lamp shows the blotches section 5 describes.
- **Card shading and the probe gather in the beauty shot run on the host.** The shot uses the device
  resolve and denoiser; its caches are updated by the host system, which costs about 17 ms of
  processor time a frame at the Colonnade's 12 782 cards and 30 to 37 ms in the sunlit room's and
  the hall's closed interiors. The device card shading and gather (stages 2 and 3) are the
  installation that removes it.
- **`samples/10-world`**: the world shades per vertex in its own shader with no depth and normal
  prepass and no shadowed sun of its own; it needs the terrain in the GI scene (cards and a
  heightfield distance grid per chunk) and its ambient replaced before it can take the GI.
- **DXR and Metal acceleration structures**: the RHI vocabulary is theirs too, and both backends
  refuse until they implement it; the software tier answers.
- **Promoted emitters in the frame's own lights**: the forward frame's clustered lights and the
  stochastic many-light path do not read `frame_lights()` yet; the frame reads the GI's two targets
  and nothing else of this system.
- **GI volumes and probe quality on the device gather**: placement, density and the update rate are
  host decisions and hold on both paths; the device gather traces the update's ray count through
  the frame's tier for every probe, so `Quality` and `TierPreference` reach probes on the host
  gather only.
- **The device GI off Vulkan**: its Metal and DXIL shaders are compiled and committed, and no suite
  has run them on a device, so the editor's viewport keeps the processor's views on Metal (the
  skinned variants' rule) and `samples/13-rts-api --render` is Vulkan only.
- **The editor's GI in Play is the document's**: Play's moving bodies do not move the GI's objects
  (the preview is rebuilt from the document on an edit, not from the simulation), and a document
  edit during Play rebuilds the GI under the running game, which loses the volumes it placed.
  Materials are a cooked material's base colour and emission, or the frame's default grey (material
  graphs light as grey). The Swift API sets no transmission and does not tune the per-pixel
  resolve's own settings (grid divisor, denoiser) yet. Every requirement of the row maps to a test
  case in `tools/roadmap/requirements-coverage.toml`; the notes there say what each does not cover
  yet.
- **The RTS field is boxes**: `samples/13-rts-api --render` draws the level as the pipeline suites'
  scene shaped into it, its units as boxes and its beacon's box unlit by its own emission (the frame
  material has none); the GI holds the same boxes in the same metres and colours.
- **A directional depth per probe for `sample`**: six axis distances cannot tell a doorway's edge
  from a wall, and those probes still leak into `sample`'s answer (the diagnostics name them);
  `sample_traced` — the per-pixel resolve's — traces each probe instead.
- **The profile in the one trace**: `GiProfile` is a structured report; it is not emitted into
  `diagnostics-profiling-and-crash`'s trace.
