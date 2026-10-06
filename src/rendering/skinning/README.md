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
  vertex by vertex. `render.skinning` executes SPIR-V through Vulkan and
  `render.skinning_metal` executes MSL through native Metal from the same source.
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

## What M11.c closed, and what it did not

Three absences were recorded here at M6 and M8.d and did not move for five milestones. All three are
closed; the record of what they were is kept because the arguments that justified them are the ones
that had to be answered.

* **Dual quaternion skinning.** Was refused by name, on the argument that a dual-quaternion skin
  needs the pose *as dual quaternions* while `PoseWorld` publishes matrices, and that deriving a
  rotation "per vertex per influence" is the wrong place by two orders of magnitude. The second half
  was right and it is an argument about WHERE: per bone it is a hundred conversions a frame, the same
  order as composing `model * inverse_bind` in the first place. `GpuBoneDualQuaternion` and
  `pack_bone_dual_quaternion` are the second representation, defined beside `GpuBoneMatrix` and
  `pack_bone_matrix` because a buffer layout the dispatch reads is the dispatch's; `PoseWorld` still
  publishes `Mat4` and nothing about it changed. The blend is DLB with the antipodality fix, which is
  not optional — `q` and `−q` are one rotation and their sum is zero — and `unit.render_geometry`'s
  *the elbow keeps its length where a matrix blend shortens it* is the candy wrapper in numbers:
  0.707 of a unit from the elbow under a matrix blend against exactly 1 under a dual one.
* **Blend shapes.** `BlendShapeSet` was the storage and the active-shape compaction and the
  arithmetic was missing. It is here now, applied to the BIND POSE before the skin — a delta is
  authored against the modelled shape — by a binary search per active shape, which is what
  `BlendShapeSet::add`'s refusal of an unsorted delta list has always been for. Only the active
  shapes are read, which is the requirement's "WHEN 50 blend shapes exist and 5 have non-zero weight"
  made mechanical rather than asserted.
* **Which instances are skinned.** `kSpatialSkinned` and `kSpatialTwoSided` are bits the broad phase
  already loads per instance, `VisibleInstance::flags` carries them out of culling, and
  `build_draw_list` writes `GpuDrawInstance::flags` through `instance_flags_of` — the one place the
  two flag enumerations are mapped, because they are different bit numbers and a copy of the word
  would have produced a plausible flags field meaning something else. Until M11.c that field was
  written by NOBODY, so every draw in this engine claimed to be unskinned.

**What was still true until issue #76 stage 3:** a caller drove one `SkinPass` per skin by hand,
with no per-instance skinning table and no dispatch that skinned a scene's worth of characters in one
submit. `SkinnedScene`, below, is that table.

## `SkinnedScene`: a scene's skinned instances in the engine's frame — issue #76 stage 3

`include/cy/rendering/skinning/skinned_scene.h` is the renderer half of the GPU pose world that
`cy::animation::PoseWorld`'s header left to the renderer ("NO DEVICE, NO BUFFER, NO UPLOAD"):

| | |
|---|---|
| One pose buffer | `upload_poses(world.matrices(), world.upload_offset(), world.upload_size())` writes the world's DIRTY RANGE and nothing else, at the world's own indices, so `PoseWorld::matrix_offset(handle)` is the `pose_offset` a dispatch reads with no translation. Both representations are written per changed bone. |
| One skinning table | `add_mesh` packs a bind pose, its frames and its influences into shared input buffers once (an eight-influence mesh's records start at twice its first vertex, so the first vertex is chosen to keep records from overlapping); `add_instance` gives an instance a window of the shared output, twice its vertex count, reused by the next instance of that size after `remove_instance`. |
| One pass | `declare(graph, frame_index)` adds one compute pass that records a dispatch per posed instance over one descriptor set, each with its own push constants, writing the half of its window the frame's parity selects. Nothing is declared when no instance has a pose. |
| The frame's draw | `skinned_draw_geometry` (`frame_skinning.h`) fills a `pipeline::DrawGeometry` with the instance's current window, last frame's window when the instance was skinned in the frame before (`SkinnedOutput::has_previous`), and the mesh's indices and UVs. `vertex_reads()` are the resources every frame stage that draws the output declares in `FramePassCallback::vertex_reads`. |

The dispatch is `skin.slang`, unchanged, and its output frame is still `render::PackedNormalTangent`:
the frame binds it as `rhi::Format::Rgba16Snorm`, the format `rhi` gained for it, through
`FramePipelines`' skinned variants (`PipelineSetup::skinned`). So the gap this README and
`skin_dispatch.h` recorded — "a skinned mesh drawn through that vertex input needs the gap closed,
and this module will not paper over it by inventing a second frame encoding" — is closed by the
format, as they asked. `SkinPass` and `SkinnedScene` create the pipeline through one function
(`src/skin_pipeline.h`).

**The depth prepass, the directional shadow, the opaque and transparent passes and the selection
mask** all draw a skinned instance from the skinning output; the prepass reads last frame's window
as `kPreviousPositionStream`, which is per-object motion for a deforming mesh.

**What it does not do:** blend shapes (a mesh with active shapes is a `SkinPass`), and frames in
flight — the same contract `SkinPass` states: the pose buffer is host-visible and the output is
double buffered by frame parity, so a host that overlaps frames keeps a scene per frame in flight.
`SkinnedScene` takes the pose as a span and a range, not a `PoseWorld`, because this module builds
with `CY_ANIMATION=OFF`.

| Suite | What it proves |
|---|---|
| `integration.skinned_scene` | the table on the null backend: one pass and one dispatch per posed instance, the constants each pushes, the output halves alternating and the previous one named only when it is last frame's, an upload writing the dirty range and nothing outside it, eight-influence records that never overlap another mesh's, a removed window reused with no other moving, and the refusals |
| `render.skinned_frame` | on Vulkan, through `pipeline_test::FrameScene` with a bent two-bone limb in place of a box: (a) the skinned pipelines created and no skinned instance is the frame `references/frame_scene_before_bloom.png` holds, byte for byte; (b) the limb's vertices are `cpu_reference_skin`'s word for word and the frame matches `tests/references/skinned_frame.png`; (c) the same with a directional shadow map, which changes when the limb bends; (d) a still limb's motion vectors are exactly zero and a moving forearm's are not while the rigid upper arm's stay zero; (e) the selection mask moves with the limb; (f) three limbs in one pass of three dispatches; (g) an upload carries a changed instance's matrices and not a sentinel staged outside the dirty range |
| `integration.render_pipeline` | on the null backend: a skinned draw binds the skinned pipelines and is recorded from vertex zero with each stream at its own offset; without the skinned pipelines it is skipped and counted; and creating them records exactly the frame without them |

Each was proven red by a mutation recorded in
`openspec/changes/add-skinned-forward-frame/evidence/falsification.md`.

`GpuCullPass`'s refusal of `kGpuCullOcclusion` remains the pattern for anything this module cannot
do: a dispatch that quietly ignored a field would be indistinguishable from one that honoured it over
empty data.
