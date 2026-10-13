# Global illumination in CyberEngine

A walkthrough for a contributor or an agent: how CyberGI is put together, how its host and device
halves meet, how the tracing tiers are chosen, how emissive surfaces light a scene, how GI volumes
override the global settings in a region, how every result is held to the host oracle and the path
tracer, and what is not built yet.

**Governed by**: [`rendering-global-illumination`](../../openspec/specs/rendering-global-illumination/spec.md)
and [`ray-tracing-infrastructure`](../../openspec/specs/ray-tracing-infrastructure/spec.md). The
module READMEs below are the reference; this guide is the route through them. Issue #35 is the plan
for the device half, in five stages.

| Where | What |
|---|---|
| [`src/rendering/gi/`](../../src/rendering/gi/README.md) | The host system and the oracle: the GI scene, the distance field, the surface and radiance caches, the tiers, the resolve, the bake's path tracer, the budget |
| [`src/rendering/gi_gpu/`](../../src/rendering/gi_gpu/README.md) | The device half: the scene upload, card lighting, the radiance cache's gather and query, the tiers |
| [`src/rendering/raytracing/`](../../src/rendering/raytracing/README.md) | The acceleration service: every decision about structures, and the mirror seam |
| [`src/rendering/raytracing_gpu/`](../../src/rendering/raytracing_gpu/README.md) | The service's structures built on a device |
| [`src/backends/rhi/`](../../src/backends/rhi/README.md) (`acceleration.h`) | Acceleration structures in the RHI, and the refusal of a device without them |
| [`src/rendering/light_probes/`](../../src/rendering/light_probes/README.md) | The irradiance volume the frame shades with today |
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
| 4 | the per-pixel resolve, the denoiser, the composite into the frame | — | not built |
| 5 | the device cost in `GiBudget` and the arbiter's ladders | — | not built |

**Nothing on the device is composited into the frame before stage 4.** The frame's indirect light
today is the irradiance volume (`cy/frame.slang`'s `probeVolumeAmbient`), and the GI-off frame is
byte-identical to the frame before each stage landed.

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

## 4. Emissive surfaces

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

## 5. GI volumes

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

## 6. Measuring it: against the oracle, and against the engines it competes with

"Better than Lumen or Unity's adaptive probe volumes" is only worth saying as numbers a suite fails on.
The claims and where each is measured:

| Claim | Measured by | Today (RTX 5060) |
|---|---|---|
| The device computes what the host computes | `render.gi_gpu`, `render.gi_gpu_tracing` | card radiance 3.7e-8, probe payloads 5.7e-8, the query 1.2e-7 mean relative |
| Hardware and software tiers agree within a stated bound | `render.gi_gpu_tracing`, `integration.render_gi_fallback` | 0.0751 against a 0.10 tolerance |
| Losing ray queries is not a visible event | `render.gi_gpu_tracing` | the switch alone moves the image 0.022; an ordinary frame 0.142 |
| No light through a wall | `integration.render_gi_pipeline` (the visibility term), `integration.render_gi_volume` | host; on the device by transcription |
| Close to the path tracer | `integration.render_gi_fallback` against `gi/bake.h` | host only until stage 4 composites |
| Fast convergence after a change | `ConvergenceTracker`, `integration.render_gi_pipeline` | host only until stage 4 |
| Temporally stable | — | **not yet measured**: an ordinary converged frame moves the probes' answer by 14 % because the gather replaces rather than accumulates; stage 4's temporal filter is where shimmer is measured and bounded |
| Emitters lit without fireflies, within a stated error | `integration.render_gi_emissive` | a promoted 30 cm lamp 0.5 % from the path tracer with a per-receiver ratio variance of 0.027 (the reference's noise); left to the cache, 4.6; a promoted panel 2.0 % |
| A region gets the quality it is marked for, within one budget | `integration.render_gi_volumes` | 3.9x the probe updates and 2x the rays in the marked interior, the frame's probe budget unchanged; no volume reaching a point is bit for bit no volume at all |
| Explicit cost | `render.gi_gpu_tracing`'s cost case | gather at budget 0.078 ms (software) / 0.043 ms (hardware); a full 1080p gather ray per pixel, escalated, 3.4 / 2.1 ms |

**The path tracer as the reference.** `PathTracer` (`gi/bake.h`) is the same tracer the lightmap bake uses, over the same GI scene. For a
reference over cards turn on `set_emission_footprints(true)`: a hit then takes the emission of the
card whose square holds it, rather than of the nearest card — without it an emitter smaller than its
neighbours' spacing loses its corners and the reference reads 5.6% low (measured, and the reason the
mode exists). Compare in the units the caches use: `irradiance()` returns the mean cosine-weighted
incoming radiance, which is a card's `direct + accumulated / albedo`.

Report the energy over a set of receivers and the per-receiver error separately: the first averages
the reference's noise out, the second includes it. `render_gi_emissive`'s `Agreement` does both,
and prints them.

## 7. Running and changing it

```sh
just build-engine
just test-integration -R render_gi            # the host system and the oracle, no device
just test-unit -R render_raytracing           # the service and its mirror, no device
just test-render -R gi_gpu                    # the device suites; skipped where there is no device
python3 src/rendering/gi_gpu/shaders/regenerate.py --slangc build/dev/Development/bin/slangc
just build-shaders --strict src samples       # every entry point for SPIR-V, MSL and DXIL
just quality-shader-headers                   # the committed headers are what the sources compile to
```

A device shader is a **transcription**: each function names the host function it transcribes and
writes it in the same order, because a reordering that is equal in exact arithmetic is a test failure
in float. Change the host function and its transcription together, regenerate the headers, and run
both suites. The [Slang guide](slang.md) has the shader rules.

## 8. Pitfalls this code has already met

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

## 9. What is not built yet

- **The composite** (stage 4): the per-pixel resolve from the radiance cache, the denoiser on the
  device, per-frame staging for frames in flight, and the forward frame's ambient term.
- **The budget on the device** (stage 5): the device cost in `GiBudget` and the arbiter's ladders.
- **DXR and Metal acceleration structures**: the RHI vocabulary is theirs too, and both backends
  refuse until they implement it; the software tier answers.
- **Promoted emitters in the frame's own lights**: the forward frame's clustered lights and the
  stochastic many-light path do not read `frame_lights()` yet — the frame reads nothing of this
  system before stage 4.
- **GI volumes and probe quality on the device gather**: placement, density and the update rate are
  host decisions and hold on both paths; the device gather traces the update's ray count through
  the frame's tier for every probe, so `Quality` and `TierPreference` reach probes on the host
  gather only.
- **The editor and Swift**: no editor view or Swift API exposes GI, its volumes or its emitter
  settings yet. The GI rows the requirements map still exempts — dynamic classification,
  transparency and media, diagnostics and debug views — are listed with reasons in
  `tools/roadmap/requirements-coverage.toml`.
