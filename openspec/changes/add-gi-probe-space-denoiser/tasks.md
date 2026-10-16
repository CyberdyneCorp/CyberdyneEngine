# Tasks

- [x] Measure where the screen probes' 1080p frame spends its time, and each denoiser candidate on the guide's axes (`smoke.gi_gpu_probe_quality`).
- [x] Add `ScreenProbeDenoiser` (default `ProbeSpace`), `filter_tiles` and `filter_reach`; `ScreenProbeGather::filter` on the host and `cyGiProbeFilter` on the device; the integration reads the filtered coefficients.
- [x] `screen_signal_config`: no cascade under `ProbeSpace`; `cyGiDenoiseTemporal` accumulates both signals and publishes on the full grid, with a history layout of its own.
- [x] `denoise::SignalConfig::history_rejection` in `Denoiser::accumulate` and both device accumulations; 0.2 under `ProbeSpace`.
- [x] Below a ray density of one, trace that fraction of the probes a frame (`screen_probe_traces`) on the host and the device, the rest keeping the probe they stand for.
- [x] Hold the device to the host (`render.gi_gpu_resolve`: the filtered coefficients, both lever positions, past the history's floor) and the host to its definition (`integration.render_gi_screen_probes`, `integration.render_denoise_filter`).
- [x] The beauty sample's comparison: `material <key> base-color`, `air none`, `--gi-reference-step`, `--gi-reference-still`, `--gi-error-map`; `content/beauty/interior.cyshot`; `just capture-gi-interior` and `tools/docs/compose_gi_comparison.py`.
- [x] Prove each new case red by a mutation (`evidence/falsification.txt`).
- [x] Update the guide's numbers and comparison, the READMEs and the requirements map; validate with `--strict`.
