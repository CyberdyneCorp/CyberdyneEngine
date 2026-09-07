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

## What is deliberately absent at M7, and where the seam is

Stated here rather than discovered from a counter that reads zero.

* **Occlusion culling.** `virtual-geometry` specifies a two-pass HZB scheme and the renderer has no
  hierarchical depth buffer yet. `TraversalStatistics::nodes_pruned_by_occlusion` and
  `rejected_by_occlusion` exist, read zero, and are the seam: the shader takes the cull tests in the
  order the requirement fixes and the HZB test slots in beside the cone test.
* **The hardware rasterisation path.** What ships is the compute rasteriser, which the specification
  accommodates explicitly ("cluster dispatch, output format, and the visibility buffer SHALL not
  assume hardware rasterisation exclusively"). The requirement's default is hardware, and that path
  is the same visibility buffer written by a vertex and fragment shader over the same records; it
  lands when virtual geometry is wired into the forward frame's pass order.
* **Assemblies.** An asset referencing other assets with transforms is specified and not built. The
  seam is `GpuAsset`: an assembly resolves to more instances of an already-registered asset, which
  is what the instance buffer already carries.
* **A suballocator in the geometry cache.** The cache is one shared allocation and a page's location
  is a byte offset into it, bumped over the accounted total. Eviction is what the requirement is
  about and is fully scored; packing is what a suballocator would add, and its tests would be about
  fragmentation.
* **A hash for the DAG visit marks.** GPU traversal marks visited nodes with one word per
  (instance, cluster). `GpuTraversal::initialise` computes the allocation and **refuses** above 64 Mi
  marks rather than overflowing on the device.
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
