# CyberGeometry — virtual geometry

M7 section 7. `virtual-geometry` at **Working**: the cooked asset and its pages, the cluster
hierarchy and its crack-free error metric, the GPU geometry cache and its streaming feedback, GPU
traversal and cluster culling, and the visibility buffer with its material resolve.

A mesh is a hierarchy of small triangle clusters, streamed on demand and selected per cluster, so
that rendering cost tracks the pixels on screen rather than the triangles in the asset.

## The one idea, and the two numbers it is made of

A cluster is selected when

```
screen_error(lod_error, lod_sphere) <= threshold < screen_error(parent_error, parent_sphere)
```

and on no other grounds. Both halves are properties of a **group** rather than of the cluster:
`lod_*` describes the group this cluster was produced from by simplifying it, and `parent_*` the
group it was simplified into. Every cluster produced from one group carries the same lower bound;
every cluster that was a member of one group carries the same upper one. That is what makes the cut
watertight, and `include/cy/rendering/virtual_geometry/cluster.h` carries the whole argument in
twenty lines.

Two consequences the builder enforces and the suite checks: a parent's error must be **strictly**
above its children's — equal errors put a hole at exactly one threshold — and a parent's sphere must
**contain** its children's, or the projection can order the two tests the wrong way round.

## What is here

| File | What it is |
|---|---|
| `cluster.h` | The vocabulary: the policy, the normal cone, the two error spheres, the projection and the selection test |
| `simplify.h/cpp` | Quadric-error edge collapse over one group, with an exact locked-vertex set |
| `build.h/cpp` | Welding, clustering, grouping, the level loop, and the watertightness check |
| `asset.h/cpp` | The page format, the quantised encoding, the derivation key, and the round trip |
| `residency.h/cpp` | The GPU geometry cache: one shared budget, scored eviction, generation counters |
| `traversal.h/cpp` | Instances, the reference traversal, and the records the shaders read |
| `gpu.h/cpp` | The compute traversal: instance cull, the iterated descent, cluster culling |
| `visbuffer.h/cpp` | The visibility buffer, material classification and binning, attribute reconstruction |
| `forward_visibility.h/cpp` | The hardware rasteriser, as the forward frame's `virtual geometry` stage |
| `shaders/` | The Slang sources and their checked-in SPIR-V |

The cook lives in `tools/cook/` (`cy/cook/geometry.h`), because a cluster hierarchy is cooked rather
than imported: the source mesh is already the engine's and what is being decided is a cook profile.

## Three defects this module's own checks found

Recorded because each is the kind that is invisible until something looks for it.

**A hash that cancelled.** Mixing three lattice coordinates with `x*A ^ y*B ^ z*C` collapses on
symmetric geometry: on a 162-vertex icosphere it produced 114 distinct identifiers, silently merging
a quarter of the mesh's vertices. In the weld that splits a shared corner in two, which unlocks a
group boundary and puts a crack in the surface. `Lattice` in `build.cpp` mixes sequentially and
**confirms** a hit against the stored cell.

**A pinch across a group boundary.** Two groups simplified independently can each create the same
new edge between two of their shared boundary vertices; the edge then carries two faces in each patch
and four in the surface. Each patch is a manifold and the per-patch check passes. Measured: four of
120 cuts of a 1,280-triangle icosphere. `Simplifier::collapse_breaks_topology` refuses a collapse
that would invent an edge between two locked vertices.

**A fence is not a barrier.** The traversal's first upload copied into its device buffers and waited
on the timeline before returning. Synchronisation validation reported
`SYNC-HAZARD-READ-AFTER-WRITE` on the first run: a host wait proves a copy COMPLETED and does not
make its write VISIBLE to a shader read. The upload is now a pass in the render graph beside the
dispatches that read it, and the graph derives the dependency — which is this module's own rule
applied to itself.

## The policies, and the numbers behind them

`virtual-geometry` requires several of these to be *policy* rather than constants, and to have their
chosen values and rationale recorded. They are, and the cooked asset carries them so that a reader
consults the asset rather than assuming.

* **Cluster size** — 128 triangles target and maximum, 192 vertices, 32 minimum. The vertex ceiling
  is 256 because a cluster's indices are stored as bytes; that is validated rather than clamped,
  because a wider index is a page format change.
* **Group size** — 8 clusters. Small enough that a group's interior is large relative to its locked
  boundary, large enough that the boundary is a small fraction of it.
* **Page size** — 128 KiB. The unit of streaming, compression, caching, content addressing and
  eviction. It is a compromise between two numbers: a page is the smallest thing a request can ask
  for, so a small page wastes a request on a fraction of a cluster group; and a page is the largest
  thing an eviction can take, so a large page evicts geometry the frame still wanted. At 128 KiB and
  the default cluster size, a page holds roughly forty clusters — about one group's worth at every
  level of the hierarchy, which is the granularity at which detail is actually chosen.
* **Quantisation** — positions at 16 bits, normals octahedral at 10, UVs at 12, each relative to the
  **cluster's** own bounds and range rather than the page's. Per cluster is what makes the cook
  cache-friendly at the granularity task 7.5 names: a cluster's bytes are a function of that
  cluster's geometry alone.

Measured on a 1,280-triangle icosphere at the suite's smaller policy (32-triangle clusters,
4-cluster groups, 4 KiB pages): 88 clusters over 7 levels in 13 pages, 50,612 bytes cooked, 18.8
bytes per triangle, a 4,072-byte resident root, and a 2.2e-5 quantisation error against a unit
sphere. Editing one vertex and recooking left **64% of clusters byte-identical**.

## What is absent, and where the seam is

Stated here rather than discovered from a counter that reads zero. The list was written at M7 as
"deliberately absent at M7"; M11.c task 4.1 closed the first entry and task 4.5 requires every
remaining one to carry its **re-entry point** rather than be quietly absorbed by a row's tier
moving.

* **Occlusion culling — SUPPLIED AT M11.c, task 4.1.** Through M10 this line read "the renderer has
  no hierarchical depth buffer yet" and the two counters read zero. `cy::rendering-hzb` is now that
  pyramid, `GpuTraversal::set_occlusion` attaches it, `TraversalView::occlusion` is the CPU half —
  the SAME `render::culling::OcclusionTester` interface `GpuCullOptions` takes — and the cull chain
  takes the tests in the order the requirement fixes: frustum, cone, screen size, occlusion.
  `nodes_pruned_by_occlusion` and `rejected_by_occlusion` are what fills them now.

  **It is the same pyramid `rendering-culling-and-lod`'s occlusion cull reads**, built by the same
  `HzbPass` and tested by the same `hzb_sample.slang`, which both shaders `#include`. Two rows were
  waiting on one piece of device work and one piece of work must not be recorded as two satisfied
  requirements — `src/rendering/hzb/README.md` carries that argument.

  Measured in `render.virtual_geometry_gpu`'s *"cluster occlusion reads the shared hierarchical
  depth buffer"*, over twenty-four views sweeping occluder depth and selection threshold: 108 node
  prunes, 11 cluster rejections, 723 clusters left visible, and no view where the device and the
  reference traversal disagreed. **Both counters are asserted non-zero over the sweep**, because a
  fixture in which only one of the two tests ever fires cannot detect the deletion of the other —
  and a single view is such a fixture: for a LEAF cluster `lod_sphere` is the cluster's own sphere,
  so the node prune answers first and the cluster test never runs.
* **The hardware rasterisation path — SUPPLIED AT M11.c, task 4.3, as a stage of the forward
  frame.** Through M11.c's first pass this line read "Nothing in the tree links
  `cy::rendering-virtual-geometry` from `cy::rendering-forward`", and the link graph was the
  measurement: every cluster ever drawn had been drawn by a harness graph with the compute rasteriser
  in it. Now `ForwardFrame` declares a `virtual geometry` stage after the depth prepass
  (`FrameFeatures::virtual_geometry`), with a one-word `R32Uint` visibility target and the frame's
  OWN depth as its attachments, and `ForwardVisibility` (`forward_visibility.h`) records it: one
  indexed-indirect draw whose instances are the visible clusters, `vgVisHwVertex` decoding the SAME
  payload through the SAME `decodeVertex`, `vgVisHwFragment` writing the SAME
  `(identity << 8) | triangle` word, and the frame's depth buffer doing the depth test. A gather and
  `vgVisHwUnpack` turn the target into the `uint2` visibility buffer, and the classification, the
  bins and the resolve are declared by the same `declare_resolve_chain` for either rasteriser. The
  module now links `cy::rendering-forward` — the edge runs that way because the frame names the
  stage and owns no renderer, as `cy::rendering-pipeline` fills its opaque pass.

  Measured in `render.virtual_geometry_forward` on an RTX 5060, over one traversal: 2,529 pixels
  covered by both rasterisers, none by one alone, every one naming the same cluster and the same
  triangle; every resolved normal matching `reconstruct_surface`; and a prepass plane at z = 0.8
  hiding exactly the part of the sphere behind it (1,325 of 2,529 pixels survive, 53% expected from
  the geometry), with validation and synchronisation validation clean. **`samples/07-fidelity` draws
  through this stage by default**, so `render.virtual_geometry_shaded` — and with it
  `m11c:virtual-geometry-image` — now photographs the forward frame; its reference was regenerated
  for that and moved 15 of 57,600 texels, 7 beyond tolerance (see that suite's header).

  **What is still not done, and its re-entry point.** The forward frame does not SHADE virtual
  geometry: no stage reads the visibility target into `FrameResources::color`, so the picture is
  still shaded by the sample on the CPU from the resolve's readback — `samples/07-fidelity/shade.cpp`
  reconstructs the position, runs the virtual shadow and does ALL of the lighting. That is why
  `m11c:virtual-geometry-image` is evidence about the traversal, the visibility buffer and the
  shadow page residency and not about the engine's shading: M11.c's gate changed only sample code
  and turned it red. A material resolve that evaluates the forward pipeline's own material program
  per bin belongs beside `cy::rendering-pipeline`'s opaque pass rather than in this module, reached
  through `FrameSinks::passes[VirtualGeometry]` in `FrameAssembly`, which nothing assembles yet.
  **Closing rung: M11.e.** It is recorded as `exempt:m11e` on `Visibility buffer and material
  resolve` in `tools/roadmap/requirements-coverage.toml` — the Forward+ half of that requirement asks
  for clusters in the frame's colour pass — and M11.d and M11.d.5 carry no renderer feature row: a
  resolve written before M11.d.5 would be written once for Vulkan and again for Metal and D3D12.
  **Nor is the per-cluster path selection built.** The requirement's architecture paragraph asks for
  a compute path `selected per cluster or per triangle by projected size`; the two rasterisers here
  are alternatives chosen per FRAME by the caller (`samples/07-fidelity`'s `FrameOptions::
  forward_frame`), with no classification of the visible-cluster list by projected triangle size,
  no split between the two paths and no merge of two partial visibility buffers. The re-entry point
  is a route chosen from the traversal's own projected-size estimate, and a case in
  `render.virtual_geometry_forward` over a scene with both sub-pixel and large clusters; it is the
  `exempt:m11e` note on `Rasterisation paths`.
  And an exact depth tie between two coincident surfaces is broken by draw order here, where the
  compute path breaks it by identity — measured stable across three processes on the artefact's
  frame, and not claimed stable in general.
* **Assemblies — STILL ABSENT AFTER M11.c, and this is its re-entry point.** An asset referencing
  other assets with transforms is specified and not built. The seam is `GpuAsset`: an assembly
  resolves to more instances of an already-registered asset, which is what the instance buffer
  already carries, so the work is a cook-side expansion and a `GpuScene::add_assembly` beside
  `add_asset` — not a change to the traversal. **`virtual-geometry` does not reach Complete while
  this is open**, and M11.c's task 4.5 is the rule that says so: a named absence is not closed by a
  row's tier moving.
* **A suballocator in the geometry cache — STILL ABSENT AFTER M11.c.** The cache is one shared
  allocation and a page's location is a byte offset into it, bumped over the accounted total.
  Eviction is what the requirement is about and is fully scored; packing is what a suballocator
  would add, and its tests would be about fragmentation. **Re-entry point**: `GeometryCache`'s
  `location` field is already the whole interface a suballocator would change, so the work is
  contained to `residency.cpp` and a fragmentation suite beside `test_residency.cpp`.
* **A hash for the DAG visit marks — STILL ABSENT AFTER M11.c.** GPU traversal marks visited nodes
  with one word per (instance, cluster). `GpuTraversal::initialise` computes the allocation and
  **refuses** above 64 Mi marks rather than overflowing on the device. **Re-entry point**: the
  refusal names the limit and the growth path in its own message, and the change is confined to the
  `visited` buffer's indexing in `vg_traversal.slang` plus the allocation in `initialise`.
* **The collision proxy.** `GeometryCookReport::collision_triangles` is a separate figure and it is
  zero: render geometry is explicitly not collision geometry, and generating the proxy is `physics`'s.
  Zero rather than the render count is the point — a mismatch has to be visible.

## Running it

```
CY_BUILD_DIR=build/<label> just build-engine
CY_BUILD_DIR=build/<label> just test-unit          # virtual_geometry: the vocabulary alone
CY_BUILD_DIR=build/<label> just test-integration   # the cook, the hierarchy, the cache, the reference
CY_BUILD_DIR=build/<label> just test-render        # the device: traversal against the reference
```

The render suite needs a GPU and skips loudly without one, naming the backend that was selected
instead. It runs with validation and **synchronisation validation** on, and asserts zero validation
errors: a frame that renders but trips validation is not a frame that works.

## Regenerating the shaders

The `.slang` sources compile to checked-in SPIR-V, because the virtual geometry path must run in a
build with no shader compiler — which is every Profile and Shipping build. Each source's header
comment carries its exact `slangc` invocation; `shaders/embed_spirv.py` writes the header, and
`just quality-format` formats it afterwards.
