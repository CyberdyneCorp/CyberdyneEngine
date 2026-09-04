# `src/rendering/scene/` — layer 4

The extract stage: an ECS world and a scene tree become a **render snapshot**, at M2's commit
boundary.

**Governed by**: `rendering-architecture` ("Simulation-to-render snapshot"). Arrived at M3, tasks
4.1.2 and 4.1.4.

## Why this is a module and not a function in the render server

`rendering-architecture`'s first requirement says the render server has **no knowledge of entities,
nodes or scripts**, which puts it at layer 2. Extraction needs a world, a query and change
detection. So the split is structural:

| | |
|---|---|
| `src/servers/render/` (layer 2) | what a snapshot **is**, and how a scene consumes one |
| `src/rendering/scene/` (layer 4) | where one **comes from**: an ECS world, the scene layer's transforms, and the commit that says when |

That is what lets `RenderServer` be tested with no world in existence, and it is a property of the
dependency graph rather than a discipline.

## The defined point is the commit boundary

`simulation-and-determinism` already defines one moment per tick at which state becomes
authoritative, and its argument is that "every consumer of authoritative state keys off it rather
than defining its own moment". A renderer that sampled the world anywhere else would be the second
consumer with its own moment.

So `SnapshotExtractor` is a `determinism::CommitObserver`: it is **called** with a `CommitRecord`
and cannot ask when the tick committed or take its own copy earlier. The record's `state_version` is
stamped on the snapshot, so a divergence report and a rendered frame line up afterwards.

## How "incremental" is actually delivered

> **WHEN** 100 000 static instances exist and 50 move **THEN** only the 50 changed instances SHALL
> be re-extracted

**Changed instances** — chunk-granular change detection over *three* components. A chunk is
re-extracted when its `WorldTransform`, its `MeshRenderer` or its `InterpolatedTransform` advanced
since the previous extraction, and skipped whole otherwise. `Query::filter_changed()` takes one
component, so the comparison is made in the body against the same `QueryChunk::version()` numbers
the built-in filter reads.

The granularity is the ECS's and is stated rather than papered over: `ecs-core` says "WHEN a
component is written THEN the whole chunk SHALL be considered changed". The suite therefore asserts
the property that is true — the chunks that did not change are not read at all — by putting the
movers in a different archetype from the crowd.

**Removed instances** — a sweep of the published set, and **only on ticks where
`World::structural_changes()` advanced**. The ECS has no destruction event log; this is the honest
check, and skipping it when nothing was created or destroyed is what keeps a moving world
proportional to what moved. Losing the `MeshRenderer` counts as a removal too, not only dying.

**Cameras and lights** — carried whole every tick. A frame has a handful of cameras and hundreds of
lights, and the bookkeeping to diff them would cost more than the copy.

## Components: registered by name, declared to the hash in the same change

`MeshRenderer`, `LightSource` and `Camera` are registered with `register_builtin()`, for exactly the
reason `src/scene/`'s twelve are: the reflection generator's annotated-header list and
`identity/manifest.toml` are not this module's to edit, and `core-type-system` says a manifest
identifier is assigned once and never guessed.

The consequence is paid immediately rather than deferred. A component registered by name is
invisible to the state hash unless something declares a schema for it — M2's carried-forward debt
1.2 — so `declare_render_state()` was written in the same change as the components. Its header
carries the classification table and the argument for each row; the two that matter:

* **A handle is `Derived`, not `Authoritative`.** A `MeshHandle` is a slot index and a generation
  the render server assigns as assets load, so two runs that load in a different order give the same
  mesh different handles. Hashing one would report a divergence between two identical worlds — the
  same failure `StateEncoding::InternedName` exists to prevent for `cy::Name`.
* **A camera is `Presentation`, every field.** Hashing where the view is would make two clients
  watching one match from different angles diverge by construction.

`MeshRenderer::importance` is `determinism::Presentation<f32>` on the field itself as well as in the
schema: the renderer computes it from screen coverage, and a gameplay system that read it back would
have made simulation depend on the camera. `read()` requires a witness and the overload does not
exist for an authoritative one, so that is a compile error rather than a divergence M9 has to find.

## Determinism

design.md §6. The snapshot's arrays are filled in query iteration order — archetype, chunk, row —
which is a function of the world's construction. The published set is a **sorted array** of stable
ids rather than a hash map, so the removal list is ordered by construction. `test_extract.cpp` runs
two identical worlds and compares the two snapshots element by element.
