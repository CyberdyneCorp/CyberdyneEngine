## 1. The format and the shaders

- [x] 1.1 `rhi::Format::Rgba16Snorm` on Vulkan, Metal and D3D12, appended to the enumeration.
- [x] 1.2 `cySkinnedDepthVertex` and `cySkinnedForwardVertex` appended to `cy/frame.slang`; the frame's committed modules regenerated with only additions; `just quality-shader-headers` and `just build-shaders --strict src samples` green.

## 2. The frame

- [x] 2.1 `PipelineSetup::skinned` and `FramePipelines::skinned_pipeline`.
- [x] 2.2 `DrawGeometry::skinned_positions`, `skinned_frames`, `static_vertex_offset`; the recorder's skinned draws in the depth, shadow, opaque and transparent passes; `RecorderReport::skinned_draws`; `bind_draw_positions`.
- [x] 2.3 The selection mask draws skinned positions and declares the skinning output's reads.

## 3. The scene skinning pass

- [x] 3.1 `SkinnedScene`: the dirty-range pose upload, the mesh and instance table, one pass of per-instance dispatches, `SkinnedOutput`; `SkinPass` and `SkinnedScene` share one pipeline creator.
- [x] 3.2 `frame_skinning.h`: an instance as a frame draw.

## 4. Tests

- [x] 4.1 `integration.skinned_scene` on the null backend.
- [x] 4.2 `render.skinned_frame`: off identical to `frame_scene_before_bloom.png`; the reference skin and a golden image; a golden image with a directional shadow and a shadow map that moves with the limb; motion vectors of a still and a moving limb; the selection mask; one pass for three limbs; the dirty-range upload.
- [x] 4.3 `integration.render_pipeline`: the skinned pipelines bound for a skinned draw from vertex zero, the draw skipped without them, and the commands unchanged with them and no skinned draw.
- [x] 4.4 Each proven red by a recorded mutation (`evidence/falsification.md`).

## 5. The sample and the documents

- [x] 5.1 `samples/09b-animated-character` draws through the forward frame; its private pipeline and shader are removed; it gates on every frame drawing the character in three passes.
- [x] 5.2 `docs/guides/animation.md`, the skinning, pipeline and animation READMEs, the sample README, `requirements-coverage.toml`.
- [ ] 5.3 The iOS capacity scene on real skeletons and its iPhone 16 re-measurement (needs an Apple device).
