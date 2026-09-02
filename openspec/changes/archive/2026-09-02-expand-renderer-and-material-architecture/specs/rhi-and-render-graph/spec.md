## MODIFIED Requirements

### Requirement: Descriptor management
**Bindless SHALL be the default resource model.** Textures, samplers, and buffers SHALL be
addressed by index into global descriptor arrays, and shaders SHALL reach resources through the
GPU scene and the GPU material table rather than through per-draw binding.

This is architectural, not an optimisation: draw workloads generated on the GPU from the GPU
scene have no CPU in the loop to bind a descriptor set per draw. GPU-driven rendering requires
bindless.

The RHI SHALL support classic descriptor sets as a **compatibility path** for devices lacking the
required capabilities. The compatibility path's limitations SHALL be documented: it cannot execute
fully GPU-generated draw workloads, and therefore constrains virtual geometry and GPU-driven
culling to a CPU-submitted approximation with reduced instance and cluster counts.

Where a backend's bindless model differs from the engine's, the RHI SHALL emulate the engine's
model rather than exposing the difference upward.

#### Scenario: Bindless material access
- **WHEN** shading reaches a material
- **THEN** it SHALL index the global descriptor arrays through the material table, so a material
  change requires no descriptor rebinding

#### Scenario: Fallback path
- **WHEN** the device lacks the required bindless capabilities
- **THEN** the engine SHALL use the compatibility path with per-material descriptor sets, the
  renderer's structure SHALL be unchanged, and the reduced GPU-driven capability SHALL be reported
  rather than silently degrading

#### Scenario: Backend differences do not leak
- **WHEN** a backend expresses bindless differently from the engine's model
- **THEN** the RHI SHALL emulate the engine's model, and renderer code SHALL be unaware of the
  difference
