# Tasks

- [x] `gi_gpu::GpuGiFrame`: the device half over a host's system, attached as the frame's
      `ScreenSpaceGi` stage; `FrameAssembly::set_global_illumination`,
      `AssemblyDescription::normal_prepass` / `FrameFeatures::normal_prepass`.
- [x] `gi_gpu::GpuGiDebugView`: the nineteen views on the device (`gi_debug_views*.slang`), the
      automatic exposure from the frame's own indirect diffuse, the leak suspects' discs; the probe
      record's error written by the host upload and the device gather.
- [x] `smoke.gi_gpu_live`: on and off byte for byte, an emitter rule and the mode changed between
      frames, every view against the host's over the frame's surfaces, the cost.
- [x] The editor: `AuthoredFrame::set_global_illumination` / `set_device_gi_view`, the runtime's
      `prepare_gi`, the GI's sky from the frame's ambient (applied in place by the preview), ABI 1.9
      bound in Play; `smoke.editor_authored_frame_vulkan`'s device GI case.
- [x] `samples/13-rts-api`: the level's GI (`host/illumination.*`), ABI 1.9 bound, the commander's
      base light (`game/Lighting.swift`), `--render` (`host/render.*`); `integration.rts_api_sample`'s
      ABI 1.9 case and `smoke.rts_api_gi`.
- [x] Prove each new case red by a mutation (`evidence/falsification.txt`).
- [x] READMEs, `docs/guides/global-illumination.md` ("Turn on GI in your game"),
      `docs/guides/swift.md`, `requirements-coverage.toml`; the screenshots.
- [ ] The device GI on Metal and Direct3D 12: the shaders are compiled and committed, no suite has
      run them on a device, so the editor keeps the processor's views there.
- [ ] The device's card shading and probe gather in the editor and the game (the caches are the
      host system's, as the beauty shot's are).
- [ ] Moving objects in the editor's Play reach the GI (the GI is the document's, rebuilt on edits).
