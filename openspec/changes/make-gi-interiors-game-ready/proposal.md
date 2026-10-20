# Make CyberGI game-ready indoors

## Why

After the probe-space denoiser, `just capture-gi-interior` photographed a sunlit room beside the
path tracer: CyberGI was 0.193 from it (the reference's own noise 0.056) where the Colonnade
outdoors was 0.022 (noise 0.014), the surfaces the sun's patch of floor lights were 23 to 32 % too
dark, and the GI cost 6.63 ms of the RTX 5060 at 1920x1080, 3.84 ms of it the screen probes' trace.
Issue #35 asks for the darkness found and fixed against the path tracer, for an interior within
about 4 ms at the default settings with no worse error, leak, stillness or settle, and for a
game-like interior beside the path tracer.

## What changes

- **A card's direct light is the mean over its square** (`gi::shaded_card_direct`,
  `gi::card_light_visibility`; `giShadedCardDirect` on the device): each light's share of the card's
  square from shadow tests at the 2x2 sample centres, refined to the 4x4 where they disagree. A card
  was shadow-tested at its centre, so a sunlit patch a few cards across kept or lost whole cards by
  where their centres fell: in the sunlit room 1.5 m^2 of lit cards stood for 2.3 m^2 of sunlight.
- **A software shadow ray ends where the field does** (`DistanceField::reach_bounds`): past the
  box every brick lies in, every step reads a far value and finds nothing, and a ray to the sun
  spent the rest of its steps there. The same answer as the whole march, for the four shadow rays
  a card now takes per light.
- **The device card lookup reads one cell** (`gi::CardNeighbourGrid`): each cell of the lookup grid
  lists every card within the lookup radius of it, with the card's position beside its handle, so a
  hit reads one list where it read 27 buckets — the same card `CardSnapshot::find` names.
- **A probe of at most 32 rays is traced by a 32-lane workgroup** (`cyGiProbeTraceNarrow`,
  `cyGiProbeTraceHardwareNarrow`), and only the trace variant the settings use is compiled.
- **A resolve created for readback copies only the frames that ask**
  (`GpuScreenResolve::set_readback_frames`): the reference's copies were inside the timed stages.
- **The beauty sample**: glowing decals light the GI's cards and stay in the cache; the reference
  emits over each card's square; `--gi-probe-rays`, `--gi-ray-density`, `--orbit-degrees`,
  `--gi-reference-bounces`; the flat sky term's error beside the GI's; and the workshop hall
  (`content/beauty/hall.cyshot`) in `just capture-gi-interior` and `just capture-gi-hall-video`;
  the GI timed over 240 frames after the still with its host cache held (`--gi-timing-frames`), so
  the figure is the GI's and not the device's power state.

## Measured (RTX 5060, 1920x1080)

The beauty sample's GI time is the fastest of 240 frames after the still with the host's cache
held, at full clocks; "before" is the #124 binary timed at full clocks over its frames before the
still (its caption's 6.63 ms included the reference's copies and, in some runs, half memory clock).

| | before | after |
|---|---|---|
| the sunlit room against the path tracer (noise 0.056) | 0.193 | 0.091 |
| the sunlit room's GI, every stage, hardware / software | 6.24 / 9.28 ms | 3.49 / 4.61 ms |
| the workshop hall against the path tracer (noise 0.088), and its GI, hardware / software | — | 0.148, 3.78 / 5.40 ms |
| the Colonnade against the path tracer (noise 0.014), and its GI | 0.022, 4.09 ms | 0.023, 2.73 ms |
| the GI test room, hardware / software (`smoke.gi_gpu_probe_quality`) | 2.75 / 3.63 ms | 1.99 / 2.51 ms |
| its error against the path tracer, 220 points | 0.064 / 0.071 | 0.064 / 0.072 |
| a sealed room behind 10 cm | 0 / 0 | 0 / 0 |
| a still view, per frame | 0.0011 / 0.0016 | 0.0011 / 0.0016 |
| settle on a moved light | 10 / 9 frames | 10 / 9 frames |

## Scope

The surface cache's direct light, the device card lookup and probe trace, the resolve's readback,
and the beauty sample's captures. Not in this change: the contact edges the furnished rooms are
still too bright at, below the probes' spacing; the frame reading promoted emitters; and moving the
beauty sample's card shading and probe gather onto the device.
