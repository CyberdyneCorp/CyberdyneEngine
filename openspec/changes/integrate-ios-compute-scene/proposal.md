# Proposal

## Why

GPU skinning and GPU VFX execute correctly through Metal, but the iPhone presentation still draws
only the terrain shader. The mobile result therefore does not prove that either compute workload
can participate in a presented frame or what that combined frame costs on physical hardware.

## What Changes

- Run skinning and VFX simulation in the iOS frame graph before presentation.
- Draw the skinned character and GPU-resident particles over the existing terrain and day/night
  scene without CPU particle readback.
- Publish machine-readable workload markers, a physical-device screenshot, and FPS evidence while
  retaining the terrain-only measurement as the baseline.

## Capabilities

### New Capabilities

- `ios-compute-presentation`: Defines the combined iPhone skinning, VFX, and presentation path.

### Modified Capabilities

- `vfx-system`: Exposes device buffers and current graph resources needed by a renderer.
- `mobile-platform-support`: Measures the combined compute and graphics workload on iPhone.

## Impact

The iOS shipping sample, physical-device evidence runner, VFX GPU pass interface, iOS preset, and
mobile build documentation are affected.
