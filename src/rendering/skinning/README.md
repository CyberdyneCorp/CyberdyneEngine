# `cy::rendering-skinning` — the skinning compute pass

Layer 4. M8.d.

`rendering-geometry-and-resources` — "Skinning":

> Skinned meshes SHALL be transformed by a **compute pass** writing into per-instance output vertex
> buffers, so the result is reusable across passes (depth, shadows, main) without re-skinning …
> Bone matrices SHALL be read from the **GPU pose world**.

## What was missing

Before this module the engine could *describe* a skin (`render::geometry::SkinningDescriptor`),
compute its animated bounds (`skinned_bounds`), store its blend shapes (`BlendShapeSet`), evaluate a
skeleton to a pose and publish the skinning matrices (`cy::animation::PoseWorld`, `publish_pose`) —
and could not move one vertex. There was no skinning shader in the tree and no CPU skinning either;
`SkinningDescriptor` was constructed nowhere outside its own tests. `skinning.h`'s closing note about
M8.b reads as though skinned meshes were drawn. They were not.

This module is the arrow from a bone matrix to a moved vertex.

## The split, and where the arithmetic lives

| | |
|---|---|
| `src/servers/render/geometry/skin_dispatch.h` | layer 2: the layout of every buffer the dispatch binds, and `cpu_reference_skin` — a CPU implementation of exactly the algorithm the shader runs |
| `shaders/skin.slang` | the dispatch, written expression for expression in the reference's order |
| `include/.../skin_pass.h` | the device, the compute pipeline, the buffers, and the graph declaration |

That is `src/rendering/gpu_culling/`'s shape against `src/servers/render/culling/`, for the same
reason: a compute pass needs a device and layer 2 may not name one, and a renderer whose skinning can
only be verified by looking at a picture is a renderer whose skinning is verified by nobody.

The chain of evidence is **paper → reference → dispatch → pixels**:

* `src/servers/render/geometry/tests/test_skin_dispatch.cpp` checks the reference against values
  worked out on paper — an elbow at (1, 0, 0), a quarter turn, a vertex at (2, 0, 0) landing at
  (1, 1, 0). It needs no GPU and runs in every profile.
* `tests/test_skin_pass.cpp` checks the dispatch against the reference by comparing output buffers
  vertex by vertex.
* `tests/test_skin_draw.cpp` binds those output buffers as vertex buffer 0 of a graphics pipeline and
  checks the covered pixels against addresses computed from the pose.

## The output is a vertex buffer

Positions are `render::VertexStream::Position` unquantised at stride 12 and frames are
`::NormalTangent` at stride 8 — the bytes a vertex input binds, in a buffer created with
`BufferUsage::Vertex | BufferUsage::Storage`. Nothing repacks. A depth prepass, the shadow cascades
and the opaque pass bind the same range, which is what the requirement's "reusable across passes"
means.

**The barrier between the dispatch and the draw is the graph's.** `declare()` adds a compute pass that
WRITES the output; a caller's draw pass READS it with `Access::VertexAttributeRead`. This module emits
no barrier. That is also why a skinning dispatch hung off `FrameRecorder`'s Prepare-stage
`PassExtension` would be wrong: an extension sits outside the graph's declared resources and no
barrier would be derived for what it wrote.

**Double buffering is not frames in flight.** One buffer holds both frames' vertices and
`SkinnedBuffers::for_frame` picks the parity, so motion vectors can read the previous frame's
positions. One `SkinPass` still owns one descriptor set naming one set of buffers, so two frames in
flight over one pass is a genuine write-after-write the graph cannot see — the remedy is a pass per
frame in flight. The suite drains each frame before beginning the next, and says so.

## What it does not do

* **Dual quaternion skinning.** `SkinningMethod::DualQuaternion` is refused by name. A dual-quaternion
  skin needs the pose *as dual quaternions* and `PoseWorld` publishes matrices; deriving a rotation
  per vertex per influence in the dispatch is the wrong place by two orders of magnitude, and adding
  a second pose representation is `animation-and-skinning`'s to add.
* **Blend shapes.** `BlendShapeSet` is the storage and the active-shape compaction; applying the
  deltas belongs in this same dispatch and is not written.
* **Anything that decides WHICH instances are skinned.** There is no per-instance skinning table, no
  `kSpatialSkinned` bit and no route from `render::kInstanceSkinned` to a shader; `GpuDrawInstance::flags`
  is still never written by `build_draw_list`. A caller drives one `SkinPass` per skin by hand.

Both refusals follow `GpuCullPass`'s refusal of `kGpuCullOcclusion`: a dispatch that quietly ignored a
field would be indistinguishable from one that honoured it over empty data.
