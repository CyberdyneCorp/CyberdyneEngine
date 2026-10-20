# Tasks

- [x] Trace the sunlit room's darkness to its cause: the floor's cards carried 0.67 of the sunlight through the window, because a card was shadow-tested at its centre (`integration.render_gi_pipeline`'s window-light cases, red on that code).
- [x] `card_light_visibility` and `shaded_card_direct` (2x2 sample centres over the card's square, 4x4 where they disagree); `SurfaceCache::shade` uses them, and `giShadedCardDirect` transcribes them for the device card shading.
- [x] Hold the device to the host on a shadow edge (`render.gi_gpu`, the courtyard: cards wholly shadowed, crossed, and lit) and regenerate the debug views' goldens the surface cache's light moves.
- [x] Take the screen probes' trace apart in the furnished room, one thing removed at a time: the card lookup each hit resolves through, not the rays or the stand-in.
- [x] `gi::CardNeighbourGrid` and the one-cell device lookup, held to `CardSnapshot::find` card for card (`integration.render_gi_pipeline`, red when a card is listed only in its own cell).
- [x] The probe trace in 32-lane workgroups for probes of at most 32 rays (`cyGiProbeTraceNarrow`, `cyGiProbeTraceHardwareNarrow`), held to the host by `render.gi_gpu_resolve`; only the variant the settings trace with is compiled.
- [x] `GpuScreenResolve::set_readback_frames`: a capture copies back only the frame it measures, so the frames it times are the frames a game runs.
- [x] The beauty sample: glowing decals light the GI's cards and stay in the cache, the reference emits over each card's square, `--gi-probe-rays`, `--gi-ray-density`, `--orbit-degrees`, `--gi-reference-bounces`, and the flat sky term's error printed beside the GI's.
- [x] The workshop hall (`content/beauty/hall.cyshot`), `just capture-gi-interior` photographing it, the comparison's captions naming every panel's figures, and `just capture-gi-hall-video`.
- [x] Time the beauty sample's GI over 240 frames after the still with its host cache held (`--gi-timing-frames`, `Stage::set_gi_hold_cache`): over the frames before the still the driver left the device at half its memory clock in some runs, and the sunlit room timed 4.81 or 3.49 ms with one binary.
- [x] A software shadow ray's march ends where the field's bricks do (`DistanceField::reach_bounds`), the same answer as the whole march (`integration.render_gi_pipeline`'s reach case): the card's four shadow tests had taken `render.gi_gpu_frame`'s case (b) from 4.85 to 5.02 s of CPU against its 5 s budget; 4.88 s now.
- [x] Prove each new case red by a mutation (`evidence/falsification.txt`).
- [x] Measure the default on the guide's axes (`smoke.gi_gpu_probe_quality`) and the three captures; update the guide, the READMEs and the requirements map; validate with `--strict`.
