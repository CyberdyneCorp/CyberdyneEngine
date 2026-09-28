# Tasks

- [x] Prove an unchanged reimport runs no unwrap through the cook cache and the build graph, and cache the unwrap by geometry key for a reimport whose geometry did not change.
- [x] Add `lightmap_bake::pack_atlas`: shared pages, per-object resolution scaling, the block grid, the gutter, and the one-word address.
- [x] Add `MeshSceneTracer` and `bake_lightmaps`: rasterisation, per-texel path tracing through `gi::PathTracer`, emission, alpha-tested and transparent occlusion, buried texels, denoising, dilation, seam reconciliation, the three encodings, and the seeds from the same run.
- [x] Add the cooked lightmap payload and the `lightmap` build-graph producer.
- [x] Fix `exclusion_for(Baked, …)` for a lightmapped surface, with a regression case.
- [x] Append the lightmap words to `CyFrameData` / `FrameViewData`, add the fourth forward stream and `lightmapAmbient` to `cy/frame.slang`, and regenerate the frame's committed SPIR-V and MSL.
- [x] Add `cy::rendering-lightmaps`: the planes' upload, `write_lightmaps` and `frame_ambient_source`.
- [x] Add `integration.render_lightmap_bake`, `unit.render_lightmap_atlas`, `unit.lightmap_frame` and `render.lightmaps`, each case proved red by a mutation.
- [x] Pin the frame without a lightmap to a committed reference drawn by the pre-change frame shader.
- [x] Publish before/after images with the bake time and the atlas memory, update the READMEs and the requirements map, and validate this change.
- [x] Store the directional and SH L1 planes as the tilt response of the texel's light, so a normal tilted toward a bright wall brightens as the path tracer says.
- [x] Denoise over a short cascade, solve seams by conjugate gradients, find seams between objects as well as charts, and dilate one chart at a time until nothing more can be filled, with a regression case for the buried strip.
- [x] Record every mutation that turns a suite red in `evidence/falsification.txt`.
