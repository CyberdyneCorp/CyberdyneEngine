# Tasks

- [x] Split the gathers' shared inputs into `gi/screen_inputs.h`; add `GiGather` (default `ScreenProbes`) and `ScreenProbeSettings`.
- [x] Add the host oracle `gi/screen_probes.h`: layout, adaptive placement, the guided mixture, harmonics, best-match reprojection and blend, origin relocation, per-pixel integration; `ScreenResolve::produce` runs it under the lever.
- [x] Add the device passes `gi_screen_probes_common.slang`, `gi_screen_probes.slang`, `gi_screen_probes_hardware.slang`, regenerate the headers, and run them from `GpuScreenResolve` with per-stage timestamps.
- [x] Cap the shared denoiser's cascade for the probes' signal (`screen_signal_config`), on the host and the device.
- [x] Write `integration.render_gi_screen_probes` (placement, repair, history, blend, unbiasedness, lever), `smoke.render_gi_screen_probes_quality` (the four axes at 128x72), the probe cases of `render.gi_gpu_resolve`, `render.gi_gpu_frame`'s (d) and (e), and `smoke.gi_gpu_probe_quality` (the four axes and the cost at 1080p and for a 4K output, both tiers).
- [x] Prove each new case red by a mutation (`evidence/falsification.txt`).
- [x] Update the READMEs, `docs/guides/global-illumination.md` (the numbers and the comparison) and the requirements map; validate with `--strict`.
- [ ] The screen tier in the probes' rays (the resolve has no previous frame's colour).
- [ ] The full-resolution integration within ~4 ms at 1080p: the shared denoiser's full-resolution passes.
