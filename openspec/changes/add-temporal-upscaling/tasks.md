# Tasks

- [x] Split the forward frame into a render extent and an output extent, with a temporal stage producer, an executed hook, an opaque copy producer and depth and motion resampled for the stages after it.
- [x] Carry the render extent through `FrameAssembly` (with `set_render_extent` for dynamic resolution), the temporal framework's jitter and `pipeline::FrameRecorder`.
- [x] Add `src/rendering/upscale/`: modes, extents, jitter phases, dynamic resolution, the upscaler interface and registry, the host reference, `TemporalUpscalePass` and its committed SPIR-V and MSL.
- [x] Fix the render graph's barrier for a read after a read-made layout transition, with a regression test proven red on the old derivation.
- [x] Pin the frame without an upscaler to references written by the tree before the change (`render.upscale_off`).
- [x] Add `unit.`, `integration.rendering_upscale` and `render.upscale`, and prove each image case red by a mutation.
- [x] Take `--upscale` in `samples/12-beauty` and `samples/10-world`; measure GPU time and error against native at 1440p and 4K.
- [x] Publish before/after images and crops, update the READMEs, the guides and the requirements map, and validate this change.
