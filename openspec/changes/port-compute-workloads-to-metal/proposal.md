# Proposal

## Why

The Metal RHI supports compute pipelines and indirect dispatch, but the skinning and VFX passes
always submit embedded SPIR-V. Metal rejects those modules, so the engine reports compute support
while two major compute workloads cannot create their pipelines on macOS or iOS.

## What Changes

- Add one shared package for checked-in shader forms and select the form from the device's declared
  native shader format.
- Ship MSL for the skinning compute pass and verify its results against the CPU reference on Metal.
- Ship MSL for the fixed VFX scheduler kernels and accept a cooked MSL emitter kernel.
- Make unsupported target payloads fail before pipeline creation with a diagnostic naming the
  missing format.
- Record physical Apple GPU evidence and add the work to M11.e's mobile scope.

## Capabilities

### New Capabilities

- `portable-compute-workloads`: Defines native shader packaging and Metal execution evidence for
  compute-driven renderer systems.

### Modified Capabilities

- `animation-and-skinning`: GPU skinning executes through Metal as well as Vulkan.
- `vfx-system`: GPU particle simulation executes through Metal as well as Vulkan.

## Impact

The RHI shader-module input helpers, skinning pass, VFX GPU pass, generated shader headers, Metal
tests, M11.e tasks, and mobile documentation are affected.
