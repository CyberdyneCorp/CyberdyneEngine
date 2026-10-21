# Tasks

- [x] `gi_gpu::GpuGiCaches`: the card shading and the probe gather installed through the system's
      seams, declared once a submission and timed; `GpuGiFrame` runs on it, and the beauty shot
      installs it after converged mode (`--gi-caches device|host`).
- [x] The card shade a workgroup a card (`gi_cards.slang`), the rays combined in the host's order;
      `render.gi_gpu`'s device-against-host cases unchanged; the headers regenerated.
- [x] The host's decisions bounded by what changed: the partitioned selection
      (`ranked_selection.h`), the still field scroll and probe placement, no card snapshot behind a
      backend, lazy counts, `CardNeighbourGrid::update`, the culled brick solve — each held to the
      computation it replaces (`unit.render_gi`, `integration.render_gi_pipeline`).
- [x] Live timing in the beauty sample (`gi live`): the three furnished scenes with the GI's sun
      turning and a block moving, the caches on the device and on the host
      (`evidence/live-timing.txt`).
- [x] `cy::rendering-gi-streaming`: `GiCellStreaming` over `world::CellEventQueue`, cards in chunks,
      fields, instances and volumes a budget a tick, evictions charged first;
      `smoke.render_gi_streaming` (a real `WorldStreaming` route against a system that never
      streamed) and `smoke.gi_gpu_streaming` (the device incremental, no hitch).
- [x] GI volumes by owner (`GiVolumeOwner`), only changed volumes re-placing probes;
      `integration.render_gi_volumes`.
- [x] Promoted lamps in the frame's lights: `append_promoted_frame_lights`,
      `LightDescription::cosine_falloff`, `kLightEmitter`; `render.gi_gpu_frame` (f) to (h); the frame
      shader headers regenerated, every render suite's reference unchanged.
- [x] The editor's Play: `follow_play`, `end_play`, `rebind_play`, `IlluminationAdapter::rebind` /
      `restore`; `integration.editor_window_play_gi`.
- [x] Prove each new case red by a mutation (`evidence/falsification.txt`).
- [x] READMEs, `docs/guides/global-illumination.md` (sections 2, 5, 6, 6e, 6g, 6h, 7, 7b, 9, 10),
      `requirements-coverage.toml`.
- [ ] The device GI on Metal and Direct3D 12: every shader compiles for both, no suite has run them
      on a device (checklist on the pull request).
- [ ] A `SlowDynamic` object's flush on the device: its brick re-solve is still the host's (the 95th
      percentile of the live timing).
- [ ] Settings and emitter rules by owner, as volumes are: an editor edit during Play still re-applies
      the document's over the game's.
