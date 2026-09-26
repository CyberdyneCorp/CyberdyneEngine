# Tasks

- [x] Add `gi::IrradianceVolume`: the grid, the SH L1 capture through `SceneTracer` / `RadianceLookup` / `SkyTerm`, validity, axis distances, and the update policy.
- [x] Add `gi::BoxProxyScene` so a volume can be captured from proxy boxes.
- [x] Add `integration.render_gi_volume`: the capture against a CPU reference integration, colour bleeding near and not far, an unlit wall lit by bounce, the visibility term, the update policy, and a capture through the composed system's own tracer and surface cache.
- [x] Append the volume's fields to `CyFrameData` / `FrameViewData`, add `probeVolumeAmbient` to `cy/frame.slang`, and regenerate the frame's committed SPIR-V and MSL.
- [x] Add `cy::rendering-light-probes`: the probe texture and `write_probe_volume`.
- [x] Add `render.light_probes` and prove each frame case red by a mutation.
- [x] Pin the frame without a volume to a committed reference rendered by the pre-change frame shader.
- [x] Publish the before/after images, update the READMEs and the requirements map, and validate this change.
