# Tasks

- [x] `FramePassKind::DepthOfField` between the temporal resolve and bloom, declared through
      `FrameStageDeclaration`; `ScreenSpaceStageInputs::source` and `FrameResources::depth_of_field`;
      the refusals; `FrameAssembly`'s mapping and `FrameSinks::depth_of_field`.
- [x] `depth_of_field/focus.h`: the constants, the radius in pixels, the depth's inverse, the
      aperture's polygon and the gather's taps, each the twin of a `dof_common.slang` function.
- [x] Shaders: setup, tiles, dilate, gather and composite; committed SPIR-V and MSL through
      `shaders/regenerate.py`; every entry point compiles for Vulkan, Metal and D3D12.
- [x] `depth_of_field::DepthOfFieldPass`: pipelines, the five passes, their record callbacks.
- [x] Host tests (`unit.render_dof`, `unit.render_forward`) and device tests
      (`render.depth_of_field`), each image case proved red by a mutation.
- [x] Hold the frame without the stage to a reference drawn with the shaders from before the change
      (`src/rendering/depth_of_field/tests/references/depth_of_field_absent.png`).
- [x] `samples/12-beauty --depth-of-field`, the shot's focus targets, and
      `just capture-beauty-depth-of-field`; publish the stills under `docs/design/images/`.
- [x] Map "Depth of field" in `requirements-coverage.toml` to the device cases, keeping what is not
      built in the note.
- [x] Update the post, forward and depth of field READMEs.
