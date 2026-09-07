# `src/rendering/gi/` — layer 4

CyberGI: the GI scene, the sparse distance field, the surface and radiance caches, the screen,
software and hardware tracing tiers with their sample confidence, the resolve, reflections on the
same infrastructure, the reflection probes, the offline path tracer, and the GI budget.

**Governed by**: `rendering-global-illumination`, at **Working** for M7. Tasks 9.1, 9.2 and 9.5.

## The files

| File | What it holds |
|---|---|
| `scene.h` | `GiMode`, `RadianceSource`, `RadianceSample`, `Surfel`, the error targets, `select_detail_level`, and `GiScene` — cell-scoped ingestion, eviction and attributable invalidation |
| `lighting.h` | the three seams the subsystems reach each other through — `Occluder`, `IndirectSource`, `RadianceLookup` — plus `SceneTracer`, `SceneHit`, `GiLight` and the analytic `SkyTerm` |
| `distance_field.h` | camera-centred sparse clipmaps of bricks, per-asset fields composited by transform, sphere tracing and sky visibility |
| `surface_cache.h` | shaded radiance per surface card, the budgeted prioritised update, and the one line the multi-bounce approximation is |
| `radiance_cache.h` | adaptive geometry-aware probe placement, clipmap scrolling, three probe encodings, the visibility term, and the scheduler that guarantees progress |
| `tracing.h` | `ScreenTracer`, `SoftwareTracer`, `HardwareTracer`, `WorldTracer` and `TieredTracer` — one interface, cheapest sufficient tier, blended escalation |
| `reflections.h` | the roughness strategy table, the lobe distribution, and `ReflectionProbeSet` with box projection and amortised realtime capture |
| `resolve.h` | `combine()` by confidence, `exclusion_for()` (the double-counting rule), the far-field ramp, and `ConvergenceTracker` |
| `budget.h` | the GI allocation, seven priced lever ladders, the declared reduction order, and importance |
| `bake.h` | the offline path tracer, ground truth, the reference comparison, and the seeds it leaves in the caches |
| `system.h` | the composition: the named subsystems, wired, and one frame |

## Five things worth knowing before changing anything here

**The software tier is the path that runs on this machine, and that is a checkable fact rather than
a choice.** `cy::rhi::Capability::RayTracing` is an enumerator nothing sets — grep
`src/backends/rhi/vulkan/src/vulkan_instance.cpp` for `capabilities_.set(` and it is not there — so
`AccelerationService` reports `Unsupported` on every device this engine can open, and layer 4 may
not name a Vulkan header anyway. The device is not the limit: `vulkaninfo` on the RTX 5060 this was
written on reports `VK_KHR_ray_query`, and the engine does not ask for it.

The consequence is the default rather than a special case, and `tests/test_fallback.cpp` measures
what it costs. Over eighty points in a closed room, converged identically, the two tiers differ by

| | |
|---|---|
| mean relative difference, software against hardware | **0.0751** (tolerance 0.10) |
| an ordinary next frame moves the image by | 0.1903 |
| the frame that switches the profile off moves it by | 0.1514 |

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

## What is here at Working, and what is not

Not implemented, and not claimed anywhere in the code: **lightmap atlases**. UV2 unwrapping, chart
packing, border dilation and the atlas format are a cook-side pipeline nothing in the tree has at
M7. `bake.h` says so at the top of the file. A `Lightmap` radiance source exists and
`exclusion_for()` handles it correctly, so a lightmap that arrives later slots into the resolve
without changing it — but nothing here produces one.

The **screen tier's colour input is last frame's**, which is why its confidence is capped below one.
The **hardware tier executes on the CPU** for the reason above.

## What it does not depend on

No device, no render graph, no shader. Every tier is arithmetic over the GI scene, the field and the
caches; the hardware tier reaches a device through `cy::rendering-raytracing`, which does not name
one either. That is what lets every case in `tests/` run headless.
