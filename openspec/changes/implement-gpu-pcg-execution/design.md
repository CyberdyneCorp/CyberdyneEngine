# Design: portable GPU PCG conformance slice

## Boundary

The canonical record, integer mixing function, CPU generator, and digest live in `cy::pcg`.
`cy::pcg-gpu` depends on `cy::pcg` and the RHI and owns shader resources and dispatch. This keeps
the existing generator usable by cooks and servers that do not link a graphics backend.

## Workload

Each invocation maps `(seed, candidate slot)` to two 16-bit coordinates and a density through
32-bit wraparound arithmetic. It records every candidate and an accepted bit derived from an
authored threshold. Integer operations avoid floating-point contraction, denormal, and rounding
differences across GPU vendors. Output ordering is the invocation slot, never atomic append order.

The canonical digest folds seed, count, threshold, every record field, and accepted count in slot
order. The CPU and GPU use the same declared arithmetic but separate implementations, so a
transcription defect is observable.

## Shader portability

One Slang source is compiled offline to checked-in SPIR-V and MSL. The runtime chooses SPIR-V for
Vulkan and MSL for Metal through the existing RHI shader-module fields. Shipping builds therefore
do not require the shader compiler.

## Evidence and limits

The conformance test requires non-empty accepted and rejected populations, exact record equality,
equal canonical digests, and a different digest for a second seed. Today it records the Apple GPU
result. Closing `m10:pcg-gpu-domain-agreement` still requires the same executable on NVIDIA and an
artifact comparison job.

### Apple evidence — 2026-09-20

An 18-core Apple M3 Pro, arm64, macOS 27.0 (26A428), running native Metal 4 generated 4,096 records.
The GPU matched the CPU record by record: 1,518 accepted, 2,578 rejected, digest
`0x9cd2af3b172f8888`. Changing the seed by one produced matching CPU/GPU output with digest
`0xd89af118b7b412dc`.

The check was falsified by changing the shader's first mix multiplier from `0x7feb352d` to
`0x7feb352c` while leaving the CPU reference intact. The Metal run failed at candidate zero,
reported 1,553 accepted instead of 1,518, and produced `0xc92bf1c5549f090e`; restoring the generated
shader restored the passing result.
