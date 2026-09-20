# Proposal: deterministic GPU PCG execution slice

## Why

`procedural-content-generation` classifies GPU-suitable stages but dispatches none of them. The
`m10:pcg-gpu-domain-agreement` criterion therefore has no GPU output to compare, and gameplay
deterministic graphs must refuse every otherwise eligible stage.

## What changes

- Define a canonical integer-only candidate generation and density-filter operation shared by the
  CPU reference and a portable Slang compute shader.
- Add a `cy::pcg-gpu` adapter target so the core `cy::pcg` module retains its no-RHI boundary.
- Produce a canonical digest and fail on missing execution, empty output, CPU/GPU disagreement, or
  a seed-insensitive result.
- Run and record the Apple Metal leg. The same embedded Slang outputs remain usable by Vulkan for
  the later NVIDIA leg.

## Non-goals

- This slice does not add `Gpu` to `ExecutionDomain`; domains describe where generation is used,
  while CPU/GPU/hybrid describes how a stage executes.
- It does not yet schedule arbitrary compiled PCG programs or close the two-vendor criterion.
- It does not change M11.c rendering or beauty-frame code.

