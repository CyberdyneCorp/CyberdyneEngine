# Tasks

- [x] Host arithmetic: the EV100-to-stops conversion, histogram binning and adaptation deadline in
      `post/exposure.h`; the `.cube` parser, display log encoding and display-table bake in
      `post/lut.h`; the `.cygrade` look in `post/look.h`.
- [x] Shaders: `exposure_clear`, `exposure_histogram`, `exposure_adapt` and `graded_resolve`, each
      expression twinned on the host; committed SPIR-V and MSL through `shaders/regenerate.py`.
- [x] `grading::GradingRenderer`: pipelines, the table upload, the histogram and state buffers,
      the post-process callback and the metering declarations; `load_look` from disk.
- [x] Rename the post module's `CameraExposure` and `exposure_multiplier`, which collided with
      `lighting/units.h`, and add the regression case.
- [x] `FrameScene` gains an `after_assemble` hook for passes that read what the frame produced.
- [x] Host tests (`unit.render_post`, `integration.render_post_bake`) and device tests
      (`render.grading`), each image case proved red by a mutation.
- [x] Hold the frame without grading to a reference drawn before the change
      (`src/rendering/grading/tests/references/grading_absent.png`).
- [x] `samples/12-beauty --look`, the warm and cool looks, and `just capture-beauty-grading`;
      publish the stills under `docs/design/images/`.
- [x] Map "Exposure" and "Colour grading" in `requirements-coverage.toml` to the device cases,
      keeping what is not built as named exemptions.
- [x] Update the post and grading READMEs.
