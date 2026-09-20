## ADDED Requirements

### Requirement: Combined compute presentation

The iOS presentation SHALL execute GPU skinning and GPU VFX simulation before drawing their output
with the terrain and day/night scene in the same presented frame.

#### Scenario: Compute output reaches presentation

- **WHEN** a frame is presented on an iPhone
- **THEN** the character draw SHALL consume the current `SkinPass` output buffer
- **AND** the particle draw SHALL consume `VfxGpuPass` device-local particle and liveness buffers
- **AND** the render graph SHALL declare both compute-to-graphics dependencies
- **AND** no CPU particle readback SHALL be required

### Requirement: Physical-device performance evidence

The device runner SHALL publish the combined workload identity and measured presentation rate from
a connected physical iPhone.

#### Scenario: Combined scene is measured

- **WHEN** the physical-device recipe succeeds
- **THEN** the log SHALL report non-zero skinned vertices, VFX capacity, VFX dispatches, and GPU
  particle instances
- **AND** the evidence SHALL contain the FPS samples, median, range, hardware, OS, and screenshot
- **AND** a missing workload marker SHALL fail the evidence run
