# `src/rendering/assembly/` — the frame, out of the renderer's own parts

Layer 4. M8.b task 11.2.

**Governed by** `rendering-forward-clustered`, `rendering-architecture`, `rendering-post-processing`,
`rendering-lighting-and-shadows`, `rendering-culling-and-lod`, `temporal-rendering`,
`atmosphere-sky-and-clouds` and `virtual-texturing` — one module consuming eight specifications'
implementations, which is what makes it an assembly rather than a subsystem.

## Why it exists

M7's closing gate, verbatim:

> Of the modules under `src/rendering/`, `cy_rendering_forward`, `cy_rendering_material`,
> `cy_rendering_post`, `cy_rendering_shadows`, `cy_rendering_sky`, `cy_rendering_temporal`,
> `cy_rendering_gpu_culling` and `cy_rendering_virtual_texturing` are linked by **nothing but their
> own test binaries**. Nothing in the tree assembles a frame out of the renderer's own parts.

Eight suites, eight green ticks, and no frame. This module links all eight and **calls every one of
them**, once, in an order, and hands the result to `GraphExecutor`.

| Type | What it is |
|---|---|
| `SceneIndex` | a `RenderSnapshot` applied to a `SpatialIndex`, keyed by stable id. The link between the extract stage and the cull that did not exist. |
| `FrameAssembly` | one view's frame: cull, draws, lights, clusters, shadow pages, sky, the thirteen forward stages, the device passes, compiled and executed. |

## The order, and why it is that order

1. **Post decides the features.** Whether the chain has a temporal stage is what decides whether the
   prepass writes motion vectors, and `select_prepass_mode` derives that from the feature set rather
   than being told. Asking post last would mean a chain needing velocity in a frame that had already
   decided not to produce it.
2. **Temporal advances** — one jitter, applied in one place. The cull uses the *unjittered* frustum.
3. **Culling** — `cull_view` on the CPU, or `GpuCullPass` on a device that supports it. The two
   produce the same `CullResults`, which is why the choice is one branch and not two frames.
4. **The draw list** is built and sorted; `layer()` slices it by sort layer.
5. **Lights** become `GpuLight` records and cluster elements; `assign_clusters` runs once over both.
6. **Shadow pages** are requested, so the budget is spent before the frame rather than inside it.
7. **The sky table** updates, and only when the sun has moved.
8. **The forward frame** is declared, with the device passes beside it.
9. **The graph** is compiled and executed, inside the host's device frame.

## What it is not

* **Not a policy.** No quality level, no configuration asset, no project setting.
  `cy::rendering-arbiter` decides; this takes the answers as arguments.
* **Not the shaders.** A pass's record callback is the caller's, exactly as `ForwardFrame` already
  requires. What this adds is that the callback is handed a sorted draw list, a cluster assignment,
  a light buffer and a material table, instead of being expected to build all four.
* **Not a modification of `src/rendering/material/`.** That module is linked and used and untouched:
  it is M7's closed work and one extension invalidates its cook keys.

`gi/`, `denoise/`, `raytracing/` and `virtual_geometry/` are absent because they were never the
gate's complaint — they link each other, and adding them here before a consumer needs them would be
the same mistake in the other direction.

## What the frame says it did — `capture_manifest.h`, M11.c task 3.2

`rendering-post-processing` fixes the chain's order and its colour space and **nothing in it
constrains what a published picture actually ran**. The largest capture this project publishes came
out of a target that linked neither the post chain nor this module, so a caption claiming tone
mapping would have been wrong with nothing able to say so.

`capture_manifest()` is the frame emitting its own stage list. It reads `AssemblyReport::post_stage[]`
— the array `build_post_chain` wrote, in the order it wrote it — and never the `AssemblyDescription`,
because the description is what the project ASKED FOR and the report is what the frame DID. It
refuses a frame that never executed, and refuses a **publication** capture taken while the budget
arbiter was free to degrade the frame. `check_caption()` compares a caption's English against it and
fails NAMING the stage the caption claimed and the frame did not run.

## What a light's shadow mode ended up being — M11.c task 3.5

`request_shadow_pages` now selects a `ShadowMode` per light through
`cy::rendering::select_shadow_mode`, against the profile the caller declares in
`AssemblyView::shadow_profile`; walks `resolve_shadow_lookup` for every page it asks for into
`AssemblyReport::shadow_substitutions`, so "the system SHALL degrade along a defined chain" is a
number rather than a function nothing called; and reaches the `Approximation` rung only when the
CALLER says a trace is available this frame. A shadow-casting light that MOVED since the last frame
dirties its own pages through `invalidate_light` before any page is requested — which is what stops
a moving sun leaving its shadows where it was.

## The suites

| Suite | Kind | What it proves |
|---|---|---|
| `integration.render_assembly` | integration | one frame produces a number from each of the eight; every draw's material slot is checked against the material table and a draw past its end is counted; an authored `MeshRenderer` becomes a draw naming its mesh; the frame executes on the null backend; and — M11.c — the frame passes through tone mapping and through anti-aliasing, each asserted against the SAME frame assembled without it, with the caption checked against the manifest in both directions |
| `render.assembly` | render | the same frame on Vulkan with validation and synchronisation validation on; the cull dispatch and the virtual-texture resolve run inside the frame's own graph; sixteen loaded frames and then a teardown with the device still busy |

The integration suite is `integration` and not `unit` deliberately: every case builds a world, a
scene tree and a device, and the taxonomy names all three as what does not belong in `unit`.
