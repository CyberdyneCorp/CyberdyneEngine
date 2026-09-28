# Tasks

- [x] `FramePassKind::DepthOfField` between the temporal resolve and bloom, declared through
      `FrameStageDeclaration`; `ScreenSpaceStageInputs::source` and `FrameResources::depth_of_field`;
      the refusals; `FrameAssembly`'s mapping and `FrameSinks::depth_of_field`.
- [x] `depth_of_field/focus.h`: the constants, the radius in pixels, the depth's inverse, the
      aperture's polygon and the gather's taps, each the twin of a `dof_common.slang` function.
- [x] Shaders: setup, tiles, dilate, gather and composite; committed SPIR-V and MSL through
      `shaders/regenerate.py`.
- [ ] `just build-shaders --strict src samples` reports target_refusals=0 (Vulkan, Metal, D3D12).
- [ ] Build in debug and dev; clang-tidy over every new C++ file.
- [x] `depth_of_field::DepthOfFieldPass`: pipelines, the five passes, their record callbacks.
- [x] Host tests (`unit.render_dof`, `unit.render_forward`) and device tests
      (`render.depth_of_field`) written.
- [ ] Run them green, and prove each image case red by a mutation applied and restored
      (md5-verified).
- [x] Commit `src/rendering/depth_of_field/tests/references/depth_of_field_absent.png`: render.bloom's
      `frame_scene_before_bloom.png` (md5 b8a26f4c2f82f4939e3dedda3b72199b) copied unchanged, drawn
      before bloom and so before this module (this change touches no frame shader).
- [ ] Hold the frame without the stage to it: the case in `render.depth_of_field` run green.
- [x] `samples/12-beauty --depth-of-field`, the shot's focus targets, and
      `just capture-beauty-depth-of-field`.
- [ ] Capture and publish the stills under `docs/design/images/`; check the comparison boxes in
      `tools/docs/compare_depth_of_field.py` against where the targets actually project.
- [x] Map "Depth of field" in `requirements-coverage.toml` to the device cases, keeping what is not
      built in the note.
- [x] Update the post, forward and depth of field READMEs.
