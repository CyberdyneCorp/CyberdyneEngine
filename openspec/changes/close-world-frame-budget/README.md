# close-world-frame-budget

The M10 world demo now dispatches terrain substrate shading, compact visual cloud composition, and
water foam evolution as render-graph compute work. The same Slang sources produce embedded SPIR-V
and MSL, with native Metal selected on Apple platforms and Vulkan elsewhere.

The fixed Shipping take used seed `20260913`, 64 frames, and 960x540 output on an Apple M3 Pro:

- Metal: 12.08 ms mean, 14.93 ms worst, zero RHI validation errors.
- Headless authoritative simulation: 5.95 ms mean, 6.82 ms worst.
- Authoritative digest in both modes: `0x796E233B79130401`.

Raw reports are in `evidence/apple-m3-pro-metal.txt` and
`evidence/apple-m3-pro-headless.txt`. `evidence/visual-workload-falsification.txt` records known
terrain, cloud, and foam shader mutations that each made its corresponding audit fail.

The cloud pass is a seeded, layered density march over the sky dome. It uses the engine's integer
noise, weather coverage, camera position, time, and sun height, with a CPU/device agreement check.
It removes the sample's synchronous sky-colour loop, but does not claim to replace the atmosphere
module's full per-pixel spherical-shell march or its atmospheric lookup-table scattering.
