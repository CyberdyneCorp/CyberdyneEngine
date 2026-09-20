# Proposal

## Why

The M10 world demonstration spends most of each frame recomputing visual terrain substrate, sky,
and water foam on the CPU. Its recorded 114.9 ms mean and 129.4 ms worst frame
times miss the 16.7 ms shipping budget, while the GPU implementations begun in M11.a are not
dispatched by the sample at all.

## What Changes

- Dispatch terrain substrate shading, visual cloud composition, and visual foam evolution on the
  graphics device used to render the world.
- Make the world sample consume both Vulkan SPIR-V and native Metal MSL artifacts generated from
  the same Slang programs, and choose the backend from the running platform.
- Keep authoritative world state deterministic on the CPU while treating the three device results
  as visual frame data.
- Measure the fixed 64-frame world take in Shipping at 960x540 on a real device and require the
  worst frame to remain at or below 16.7 ms.
- Restate the headless budget around authoritative simulation work, since headless execution has
  no visual device workload to measure.
- Make the world run recipe compatible with the Bash 3.2 shipped by macOS.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `delivery-roadmap`: Replace the unresolved world-frame-budget gap with executable device and
  headless criteria that measure the work each execution mode actually performs.
- `terrain`: Require the integrated world frame to consume terrain substrate shading produced by
  device work rather than resampling all environment fields synchronously on the CPU.
- `atmosphere-sky-and-clouds`: Require the integrated world's visual cloud evaluation to execute
  as device work.
- `water`: Define foam used only for rendering as device-owned visual state while preserving the
  CPU's authoritative water state.

## Impact

This change affects the M10 world sample, its Slang shaders and generated artifacts, the rendering
graph passes that prepare its visual data, the M11.a/M10 roadmap ledgers, and the world capture/run
recipes. It adds no public RHI platform conditionals and changes no authoritative save, replay, or
network state.
