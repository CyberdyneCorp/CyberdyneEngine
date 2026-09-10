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

## Components: reflected at M8.b, declared to the hash since M3

`MeshRenderer`, `LightSource` and `Camera` were registered with `register_builtin()` until M8.b —
by NAME, with no `reflect::TypeId` behind them — for the reason `src/scene/`'s twelve gave: the
reflection generator's annotated-header list and `identity/manifest.toml` are not a module's to
invent, and `core-type-system` says a manifest identifier is assigned once and never guessed.

**M8.b's task 11.3 paid for the identifiers and reflected them.** The cost of not having done it
came due at M8.a, whose artefact photograph shows an authored sphere drawn as a unit box: a
component nothing could look up by type is a component `build_authoring_schema` does not carry, so
`resolve_against` could not match a `.cyworld`'s `MeshRenderer` to anything and the mesh a designer
picked reached no renderer. `components.h` carries the whole argument.

Two consequences the rest of this module is shaped by:

* **A `MeshRenderer` names an ASSET, and separately a HANDLE.** `mesh` and `material` are
  `AssetRef`s — 128-bit persistent identity, reflected, serialized, what a rename rewrites — and
  `mesh_handle` and `material_handle` are what those resolved to in this process, unreflected
  because "handles are runtime-only and are never serialized".
* **Something has to turn the first into the second**, and that is `asset_binding.h`. It takes a
  resolver rather than loading anything: where a mesh comes from is `core-assets-and-io`'s, and the
  only part that is the renderer's is the mapping from a reference to the slot this process gave
  it. A reference nothing resolves CLEARS the handle rather than keeping a stale one, because a
  slot is reused and a stale handle draws another asset's geometry.

The state schema was written in the same change as the components at M3 — M2's carried-forward debt
1.2 — because a component registered by name is invisible to the state hash unless something
declares a schema for it. Its header carries the classification table and the argument for each row;
the two that matter:

* **A handle is `Derived`, not `Authoritative`.** A `MeshHandle` is a slot index and a generation
  the render server assigns as assets load, so two runs that load in a different order give the same
  mesh different handles. Hashing one would report a divergence between two identical worlds — the
  same failure `StateEncoding::InternedName` exists to prevent for `cy::Name`. **The asset
  references beside them are `Authoritative`**, and that is the pair's whole point: an id is content
  identity and is the same in every process.
* **A camera is `Presentation`, every field.** Hashing where the view is would make two clients
  watching one match from different angles diverge by construction.

`MeshRenderer::importance` is `determinism::Presentation<f32>` on the field itself as well as in the
schema: the renderer computes it from screen coverage, and a gameplay system that read it back would
have made simulation depend on the camera. `read()` requires a witness and the overload does not
exist for an authoritative one, so that is a compile error rather than a divergence M9 has to find.

## The shipped node catalogue's renderer half

`node_templates.h`. `scene-graph-and-nodes` ships a catalogue of node types as data and
`src/scene/src/node_template.cpp` declares every one of them by component NAME — which the scene
layer cannot complete from where it sits, because a template's `defaults` blob is the component's
own bytes and layer 4's node façade may not include the renderer's header. So five of them —
`MeshRenderer`, `Camera`, `DirectionalLight`, `PointLight` and `SpotLight` — were declared and not
instantiable. `declare_render_templates()` redeclares them with real defaults and rebinds the
catalogue, and it refuses a world the renderer's components are not registered in rather than
binding every template to nothing.

## Determinism

design.md §6. The snapshot's arrays are filled in query iteration order — archetype, chunk, row —
which is a function of the world's construction. The published set is a **sorted array** of stable
ids rather than a hash map, so the removal list is ordered by construction. `test_extract.cpp` runs
two identical worlds and compares the two snapshots element by element.
