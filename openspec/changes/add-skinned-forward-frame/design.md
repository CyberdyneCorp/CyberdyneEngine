# Design

## The normal-tangent stream: a format, not a second encoding

The issue left the choice open: add `Rgba16Snorm` to `rhi`, or decode `PackedNormalTangent` in the
frame shader. Both `skin_dispatch.h` and `frame_pipelines.h` had already argued against the third
option — teaching the skinning dispatch to write the rigid streams' half-float encoding — as "a
second frame encoding for one consumer". So the format is added, and the dispatch is unchanged.

The rigid streams store the octahedral pair remapped to [0, 1] as half floats, which is the range
`decodeOctahedral` takes; a `Rgba16Snorm` fetch hands the shader [-1, 1]. Two new vertex entries move
the signed pair into the rigid range and otherwise do exactly what the rigid entries do. They are
written out rather than calling the rigid entries: an entry point that calls another entry point
crashes Slang's `translateEntryPointInParamToBorrow` pass when the module is compiled through the API
(`cy_shaderc build`, which `smoke.shader_targets` runs), and a shared helper would change the rigid
modules' bytes. The entries are at the end of `frame.slang`, so no rigid line moves and even the
rigid MSL (which carries `#line` directives) is the same text.

| | Rigid draw | Skinned draw |
|---|---|---|
| Position | the source's position stream | the skinning output, at the current window |
| Normal-tangent | `Rgba16Sfloat`, [0, 1] | `Rgba16Snorm`, the dispatch's `PackedNormalTangent` |
| UVs | the source's | the source's, at the mesh's static vertices |
| Previous position (prepass) | the position stream again | the other half of the output window |
| Pipelines | `pipeline(kind)` or a material variant | `skinned_pipeline(kind)`; the shadow pass's standard one |

## Each binding at its own offset

A skinned draw's position window, its previous window and its UVs are in three different places, so
one `draw_indexed` vertex offset cannot address them. The recorder binds every stream with its own
byte offset and records the draw from vertex zero, which also keeps the vertex offset non-negative
whichever half is current. After a skinned draw the pass's streams are rebound before the next rigid
or variant draw. A skinned draw does not consult the caller's `DrawPipelineFn`: a material variant's
vertex stage reads the rigid encoding.

## One pose buffer, one table, one pass

`SkinnedScene` is sized once (meshes, instances, input and output vertices, pose matrices) and never
reallocates on the frame path.

- **The pose.** `upload_poses(matrices, first, count)` packs `[first, first + count)` — the pose
  world's dirty range — into the device buffer at the same indices, as both matrices and dual
  quaternions, so `PoseWorld::matrix_offset(handle)` is the `pose_offset` the dispatch reads. The
  scene takes the span rather than a `PoseWorld` because the renderer builds with
  `CY_ANIMATION=OFF`.
- **The table.** A mesh's bind pose, frames and influences are packed into shared input buffers at
  `add_mesh`. The dispatch addresses influence records at `vertex * blocks`, so an eight-influence
  mesh's first vertex is chosen so its records overlap nothing already added. An instance owns an
  output window of twice its mesh's vertices; a removed window is reused by the next instance of the
  same size and no other window moves.
- **The pass.** `declare` adds one compute pass that records a dispatch per posed instance over one
  descriptor set, each with its own push constants, writing the half of its window the frame's parity
  selects. `SkinnedOutput::has_previous` is true only when the instance was skinned in the frame
  before, so a first frame or a frame after a gap does not read a stale half as motion.

The barrier between the pass and every stage that draws its output is the graph's: each stage
declares `vertex_reads()` (`FramePassCallback::vertex_reads`, and `OutlinePass::set_vertex_reads` for
the mask, which declares its own passes).

## What is not done

- Blend shapes in the scene (a mesh with active shapes stays a `SkinPass`).
- Frames in flight over one scene: the same contract `SkinPass` states.
- A bone read by another compute pass (the VFX scenario): the pose buffer is bindable; no consumer or
  test exists.
- The iOS capacity scene on real skeletons, and its re-measurement on the iPhone 16: no Apple device.
- The skinned pipelines drawn on Metal or D3D12: their modules are generated and `just build-shaders
  --strict` compiles them for every target; only Vulkan draws them here.
