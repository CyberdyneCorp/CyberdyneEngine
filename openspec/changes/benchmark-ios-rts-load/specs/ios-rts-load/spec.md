## ADDED Requirements

### Requirement: RTS skinning load

The iOS RTS presentation SHALL execute GPU skinning for 500 visible model meshes before drawing
their device-local output in the presented frame.

#### Scenario: Formation is skinned

- **WHEN** the RTS scene presents a frame
- **THEN** the skinning dispatch SHALL process 500 copies of the 36-vertex model
- **AND** the workload marker SHALL report 500 models and 18,000 skinned vertices
- **AND** the presentation SHALL consume the current skinning output buffer

### Requirement: RTS VFX load

The iOS RTS presentation SHALL simulate 100 independently resident GPU particle emitters without
CPU particle readback.

#### Scenario: Emitters are simulated and drawn

- **WHEN** the RTS scene presents a frame
- **THEN** 100 VFX passes SHALL own independent simulation buffers
- **AND** the frame graph SHALL execute 400 VFX simulation dispatches
- **AND** the draw SHALL submit all 51,200 fixed-capacity particle instances
- **AND** CPU particle readback SHALL remain disabled

### Requirement: Physical-device capacity evidence

The device runner SHALL reject an RTS run whose model or emitter count differs from the requested
load and SHALL publish the measured FPS from a physical iPhone.

#### Scenario: Exact workload is measured

- **WHEN** the RTS device recipe succeeds
- **THEN** the evidence SHALL identify 500 models, 100 emitters, their aggregate work, hardware,
  OS, FPS samples, median, range, and screenshot
