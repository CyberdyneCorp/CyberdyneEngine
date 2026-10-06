# Proposal

## Why

Issue #76 stage 3. The skinning compute pass is built and tested, and the engine's forward frame does
not draw what it produces: one `SkinPass` skins one mesh, driven by hand, from a bone buffer it fills
itself; `PoseWorld`'s dirty range is read by no renderer code; and `FramePipelines` cannot bind the
skinned normal stream, because the dispatch writes `render::PackedNormalTangent` (16-bit snorm) and
`rhi::Format` has no `Rgba16Snorm`. So `samples/09b-animated-character` drew with its own pipeline
and flat normals rebuilt from screen-space derivatives, and no shadow, motion vector or selection
outline has ever seen a skinned vertex.

## What Changes

- Add `rhi::Format::Rgba16Snorm` (Vulkan, Metal, D3D12), appended to the enumeration.
- Add `cySkinnedDepthVertex` and `cySkinnedForwardVertex` to `cy/frame.slang`, appended at the end of
  the file so every rigid module is byte-identical, and `PipelineSetup::skinned`: skinned variants of
  the depth, opaque and transparent pipelines that bind the normal-tangent stream as `Rgba16Snorm`.
- Let a frame draw name the skinning output's own buffers (`DrawGeometry::skinned_positions`,
  `skinned_frames`, `static_vertex_offset`); the recorder binds each stream at its own offset and
  records from vertex zero, through the skinned pipeline, in the depth prepass, the shadow pass and
  the opaque and transparent passes. The selection mask draws a skinned instance from its skinned
  positions (`bind_draw_positions`, `OutlinePass::set_vertex_reads`).
- Add `cy::rendering::skinning::SkinnedScene`: one device pose buffer filled from the pose world's
  dirty range, a mesh and instance table, and one compute pass that skins every posed instance with a
  dispatch each into its own double-buffered output window. `frame_skinning.h` turns an instance
  into a frame draw.
- Draw `samples/09b-animated-character` through the forward frame, with a tiled ground and a
  directional shadow, and remove its private pipeline and shader.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `rendering-geometry-and-resources`: skinned meshes are drawn by the forward frame from one scene
  skinning pass.
- `animation-and-skinning`: the renderer half of the GPU pose world.

## Impact

- `src/backends/rhi*` (one format), `src/rendering/shaders/cy/frame.slang` and the frame's committed
  modules (additions only), `src/rendering/pipeline`, `src/rendering/selection`,
  `src/rendering/skinning`, `samples/09b-animated-character`.
- A frame with no skinned draw is the frame it was: the skinned pipelines are created only on request,
  and `render.skinned_frame` (a) holds that frame to a reference drawn before the change, byte for
  byte.
