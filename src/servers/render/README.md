# `src/servers/render/` — layer 2

The handle-based render server: all renderable state, behind generational handles, with **no
knowledge of entities, nodes or scripts**.

**Governed by**: `rendering-architecture` (the server, the scene/view/instance model, the snapshot,
the GPU scene, debug visualisation and statistics) and `rendering-geometry-and-resources` (mesh
representation, vertex compression, texture formats). Arrived at M3, tasks 4.1.1–4.1.6, 4.2.1,
4.2.2.

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
