## ADDED Requirements

### Requirement: Native compute shader packages

The engine SHALL package each compute entry point in the native shader forms required by supported
RHI backends. Runtime selection SHALL use `DeviceCapabilities::native_shader_format()` and SHALL
NOT translate shaders during frame execution.

#### Scenario: Metal selects MSL

- **GIVEN** a bundle containing SPIR-V and MSL for one compute entry point
- **WHEN** a Metal device creates the module
- **THEN** the selected description SHALL contain only MSL
- **AND** the entry point and stage SHALL describe the same program as the SPIR-V form

#### Scenario: Required form is absent

- **WHEN** a device selects a native format absent from the bundle
- **THEN** selection SHALL fail before calling the backend
- **AND** the diagnostic SHALL name the missing format

### Requirement: Metal GPU skinning parity

The skinning compute pass SHALL execute through native Metal and SHALL produce positions and packed
normal-tangent frames matching the CPU reference within the existing numeric tolerances.

#### Scenario: Complete skinning feature set

- **WHEN** matrix skinning, dual-quaternion skinning, blend shapes, output offsets, and consecutive
  frames execute on a physical Apple GPU
- **THEN** every result SHALL satisfy the same comparisons used by the Vulkan suite
- **AND** Metal validation SHALL report no errors

### Requirement: Metal GPU VFX parity

The VFX GPU pass SHALL execute its generated emitter kernel and fixed reset, compact, and sort
kernels through native Metal while particle state and indirect arguments remain GPU-resident.

#### Scenario: GPU simulation agrees with the reference

- **WHEN** the reference effect executes on a physical Apple GPU
- **THEN** its population, liveness, attributes, counters, and bounded events SHALL satisfy the
  existing CPU/GPU comparison
- **AND** Metal validation SHALL report no errors
