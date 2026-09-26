# Tasks

- [x] Declare the bloom chain into the render graph and stage it in `ForwardFrame` behind
      `FrameFeatures::bloom`, publishing `post_source` for the post-process.
- [x] Carry `PostChainConfig::bloom` and `BloomSettings` through the assembly.
- [x] Write `cy/bloom.slang` (prefilter with soft knee and Karis average, 13-tap downsample, tent
      upsample, redistributing composite, anamorphic stretch, lens dirt) and commit its SPIR-V and
      MSL through `embed_bloom.py`.
- [x] Record the chain with `BloomRenderer`, attached through `FrameRecorder::set_bloom`.
- [x] Make the processor's `bloom_composite` the shader's redistribution, and add the level weights
      and the exposure-derived threshold.
- [x] Structural tests (`unit.render_forward`, `integration.render_pipeline`) and device tests
      (`render.bloom`), each proved red by a mutation.
- [x] Publish the beauty shot with and without bloom, and map the requirement in
      `requirements-coverage.toml`.
- [x] Update the post, pipeline and shader library READMEs.
