# `src/servers/render/` — layer 2

The handle-based render server: all renderable state, behind generational handles, with **no
knowledge of entities, nodes or scripts**.

**Governed by**: `rendering-architecture` (the server, the scene/view/instance model, the snapshot,
the GPU scene, debug visualisation and statistics), `rendering-geometry-and-resources` (mesh
representation, vertex compression, texture formats) and, from M5, `editor-viewport-and-gizmos` (the
editor's two entry points: picking and the viewport transport). Arrived at M3, tasks 4.1.1–4.1.6,
4.2.1, 4.2.2; extended at M5, tasks 4.1 and 4.2.

## The one rule, and the layer number that enforces it

`rendering-architecture` opens with "`RenderServer` SHALL own all renderable state and expose it
through generational handles, **with no knowledge of entities, nodes, or scripts**". That clause is
why this module is layer 2 — *below* `src/backends/` (3) and `src/rendering/` (4) — and the layer
checker fails the build over a violation rather than a reviewer catching it.

What it costs, stated once: there is no `cy::rhi` here and there cannot be. No device, no command
buffer, no image format the RHI named, no render graph. Everything is expressed in engine types —
`Transform`, `Aabb`, `Handle`, `Name`.

What it buys is the specification's own first scenario, as a property of the build rather than of a
mock:

> **WHEN** a test drives `RenderServer` directly with handles **THEN** it SHALL produce a frame
> without an ECS world or scene tree existing

`tests/test_server.cpp` includes no world, no node and no device header, because it cannot.

## What is here

| file | what it holds |
|---|---|
| `handles.h` | the twenty object families, as distinct generational handle types |
| `types.h` | the value vocabulary: texture formats, blend modes, shading models, sort layers, debug view modes |
| `model.h` | `Projection`, `View`, `Instance`, `LightDescription`, `EnvironmentSettings`, `SceneDescription` |
| `mesh.h` | surfaces, streams, vertex compression, LOD chains |
| `gpu_scene.h` | **the publication interface**: one 160-byte instance record, many producers |
| `snapshot.h` | what crosses the simulation/render boundary, and the double-buffered exchange |
| `sort.h` | the deterministic sort key, draw ordering and automatic instancing |
| `debug_draw.h` | debug primitives, double buffered, compiled out in Profile and Shipping |
| `statistics.h` | the seven frame stages, per-view and per-frame counters, memory by category |
| `server.h` | the handle pools, the scene/view/instance model, snapshot application, draw collection |
| `picking.h` | **engine-side picking**, resolved against the draw list a view produced (M5, task 4.2) |
| `viewport_transport.h` | the viewport transport's publishing endpoint: the frame's identity, its view state, its pacing (M5, task 4.1) |

## The editor's two entry points, and why they are here rather than in the editor

`editor-viewport-and-gizmos` requires picking to be **engine-side**, "so that what is picked matches
what is rendered — including virtual geometry, instanced content, foliage, terrain, and skinned
meshes", and names "editor-side picking that does not match what the engine rendered" as a forbidden
pattern. So the resolution lives here, and it is made true by construction rather than by care:

**`pick_ray` resolves against the DRAW LIST, not against the scene.** It takes the
`Span<const DrawItem>` that `collect_draws` produced for the view and considers nothing else, so the
candidate set is a subset of what was actually drawn. An instance culled, hidden, or on a layer the
view does not draw is absent because it never reached the list — and an instance published by a
producer this file has never heard of is present for the same reason. There is no second traversal to
drift out of step with the first.

**`viewport_transport.h` publishes a frame's IDENTITY and VIEW STATE alongside its image**, because
the specification requires a click to be "resolved against the view state of the frame shown, not a
newer one". `ViewportViewState::to_view()` hands back the `View` a pick should use, which is the one
thing a caller has to remember; passing its own camera instead compiles and is wrong, and that is
what the frame identifier exists to make detectable.

Neither file names a device, an encoder or a shared-handle API, because this is still layer 2. What
the editor's side of the transport measured about all this is in
`editor/crates/cy-editor-viewport/README.md`.

## Three decisions worth knowing before changing anything

**The GPU scene is a publication interface, not a mesh renderer's buffer** (design.md §4). A
producer reserves a contiguous slot range and declares who writes it; a `Residency::Gpu` range is
never touched by the CPU. That shape is for the producers arriving at M7 — VFX mesh particles,
skinned instances, virtual-geometry clusters — and one producer is the cheapest moment to get it
right. `gpu_scene.h`'s header comment names the three requirements each decision answers.

**Every draw order comes from a stable identity** (design.md §6). `InstanceDescription::stable_id`
is refused when zero, because an instance with no stable identity is one whose draw order is
publication order. Sorting reads material, mesh and depth — never a pointer, never a slot index,
never a hash map's iteration order.

**Sizes are configuration, not constants.** `RenderServerConfig` sizes the debug primitive store
before `initialize()`. The default is a game's; a test that took it would pay ~850 KiB of
construction per case, which is a millisecond at `-O0` spent measuring a default rather than a
behaviour.

## What is deliberately elsewhere

* **Extraction from the ECS** — `cy::rendering::SnapshotExtractor`, `src/rendering/scene/`. It needs
  a world, and this module is forbidden one.
* **The material model** — `cy::rendering`, `src/rendering/material/`. What is here is what sorting
  and drawing need of a material: the program, the table index, the blend mode.
* **Anything that touches a device** — `src/rendering/graph/` and `src/backends/rhi/`.
