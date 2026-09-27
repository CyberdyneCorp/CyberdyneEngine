# Tasks

- [x] Keep last frame's instance rows in `pipeline::FrameBindings`, rebased to the camera and confirmed by stable identity, and write each row's previous placement after the current ones.
- [x] Append `motionControl` to `cy/frame.slang`'s frame block, derive the prepass's motion from the previous placement and the previous vertex positions, and regenerate the frame's committed SPIR-V and MSL.
- [x] Add `DrawGeometry::previous_vertex_offset` and bind the previous positions for a mesh deformed on the device.
- [x] Add `src/rendering/motion_blur/`: tile max, neighbour max and gather, their host reference, `MotionBlurPass` and committed SPIR-V and MSL.
- [x] Add the `MotionBlur` stage to the forward frame and pass its producer and target through the assembly.
- [x] Pin the frame with neither per-object motion nor the stage to committed references rendered by the pre-change frame shaders.
- [x] Add `unit.rendering_motion_blur` and `render.motion_blur`, and prove each image case red by a mutation.
- [x] Publish before/after images and update the READMEs and the requirements map, and validate this change.
