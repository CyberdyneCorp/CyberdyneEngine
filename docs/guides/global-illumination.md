# Global illumination in CyberEngine

A walkthrough for a contributor or an agent: how CyberGI is put together, how its host and device
halves meet, how the tracing tiers are chosen, how every device result is held to the host oracle,
and what is not built yet.

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

## 4. Measuring it: against the oracle, and against the engines it competes with

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
| Explicit cost | `render.gi_gpu_tracing`'s cost case | gather at budget 0.078 ms (software) / 0.043 ms (hardware); a full 1080p gather ray per pixel, escalated, 3.4 / 2.1 ms |

## 5. Running and changing it

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

## 6. Pitfalls this code has already met

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

## 7. What is not built yet

- **The composite** (stage 4): the per-pixel resolve from the radiance cache, the denoiser on the
  device, per-frame staging for frames in flight, and the forward frame's ambient term.
- **The budget on the device** (stage 5): the device cost in `GiBudget` and the arbiter's ladders.
- **DXR and Metal acceleration structures**: the RHI vocabulary is theirs too, and both backends
  refuse until they implement it; the software tier answers.
- **The editor and Swift**: no editor view or Swift API exposes the device GI yet. The GI rows the
  requirements map still exempts — emissive classification, dynamic classification, transparency
  and media, GI volumes, diagnostics and debug views — are listed with reasons in
  `tools/roadmap/requirements-coverage.toml`.
