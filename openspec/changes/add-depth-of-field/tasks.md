# Tasks

- [x] `FramePassKind::DepthOfField` between the temporal resolve and bloom, declared through
      `FrameStageDeclaration`; `ScreenSpaceStageInputs::source` and `FrameResources::depth_of_field`;
      the refusals; `FrameAssembly`'s mapping and `FrameSinks::depth_of_field`.
- [x] `depth_of_field/focus.h`: the constants, the radius in pixels, the depth's inverse, the
      aperture's polygon and the gather's taps, each the twin of a `dof_common.slang` function.
- [x] Shaders: setup, tiles, dilate, gather and composite; committed SPIR-V and MSL through
      `shaders/regenerate.py`.
- [x] `just build-shaders --strict src samples` reports target_refusals=0 (Vulkan, Metal, D3D12):
      modules=55 entry_points=76 disagreements=0 failures=0. The committed headers are byte-identical
      to what `shaders/regenerate.py` writes.
- [x] Build in debug and dev; clang-tidy over every touched C++ file (no findings).
- [x] `depth_of_field::DepthOfFieldPass`: pipelines, the five passes, their record callbacks.
- [x] Host tests (`unit.render_dof`, `unit.render_forward`) and device tests
      (`render.depth_of_field`) written.
- [x] Run them green, and prove each image case red by a mutation applied and restored
      (md5-verified): design.md, "Mutation proofs".
- [x] Commit `src/rendering/depth_of_field/tests/references/depth_of_field_absent.png`: render.bloom's
      `frame_scene_before_bloom.png` (md5 b8a26f4c2f82f4939e3dedda3b72199b) copied unchanged, drawn
      before bloom and so before this module (this change touches no frame shader).
- [x] Hold the frame without the stage to it: the case in `render.depth_of_field` run green,
      129600 of 129600 texels byte-identical. Re-rendered on this machine the file comes out with the
      same md5; the change touches no `.slang` outside `src/rendering/depth_of_field/`.
- [x] `samples/12-beauty --depth-of-field`, the shot's focus targets, and
      `just capture-beauty-depth-of-field`.
- [x] Capture and publish the stills under `docs/design/images/`; check the comparison boxes in
      `tools/docs/compare_depth_of_field.py` against where the targets actually project (the
      sphere's centre at (0.425, 0.880), the column's at (0.568, 0.512): inside both boxes).
- [x] Map "Depth of field" in `requirements-coverage.toml` to the device cases, keeping what is not
      built in the note.
- [x] Update the post, forward and depth of field READMEs.

## Found by running it

- [x] The gather spanned the blur disc, so its outermost ring sat half a spacing inside the rim and
      six blades drew the tap grid's circle (`render.depth_of_field`'s hexagon case, red on the
      first run). It now spans the disc plus the reach's half-texel ramp: `cyDofSpan`, twinned as
      `gather_span`, with a regression case in `unit.render_dof`.
- [x] The frame's post-process and temporal resolve, and the beauty sample's resolve and shadow
      pass, bound their descriptor sets before their graphics pipeline. The Vulkan backend binds
      sets at the last bound pipeline's point, so after the stage's compute composite the resolve
      drew with an earlier pass's set 2: in the sample the particles', a validation error and a
      crash in the layer. Pipeline first now; regression case in `render.depth_of_field`, from the
      null backend's command log.
- [x] `tools/docs/compare_depth_of_field.py` filtered the cropped box, and PIL copies the edge
      pixels through unfiltered: the border counted as detail. It filters the frame, then crops;
      regression tests in `tools/docs/test_compare_depth_of_field.py`.
- [x] `Shot::focus_target`'s parameter shadowed `Shot::name` (`-Werror=shadow`).
