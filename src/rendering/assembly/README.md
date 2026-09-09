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

## The suites

| Suite | Kind | What it proves |
|---|---|---|
| `integration.render_assembly` | integration | one frame produces a number from each of the eight; an authored `MeshRenderer` becomes a draw naming its mesh; the frame executes on the null backend |
| `render.assembly` | render | the same frame on Vulkan with validation and synchronisation validation on; the cull dispatch and the virtual-texture resolve run inside the frame's own graph |

The integration suite is `integration` and not `unit` deliberately: every case builds a world, a
scene tree and a device, and the taxonomy names all three as what does not belong in `unit`.
