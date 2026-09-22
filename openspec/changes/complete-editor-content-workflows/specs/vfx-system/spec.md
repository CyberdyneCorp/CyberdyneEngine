# vfx-system Spec Delta

## ADDED Requirements

### Requirement: VFX graph services cross the editor boundary
The engine SHALL expose a deterministic, versioned VFX node catalogue plus validation, capability
query, compilation and GPU-lowering operations through `editor-backend-services`. Definitions SHALL
carry stable node, pin and property identities, typed defaults, constraints, stage metadata and
target capability requirements.

Lowering results SHALL include stable content and derivation identities, dependencies, derived
attribute layout, kernel stages, generated IR/Slang inspection, cost metadata and structured graph
diagnostics. Neither the C ABI/live payloads nor the editor SHALL expose VFX compiler, renderer,
graphics API or Metal implementation types.

#### Scenario: Backend-only VFX node appears
- **WHEN** a compatible runtime adds a VFX node definition
- **THEN** the same editor build SHALL present it from the returned catalogue

#### Scenario: Unsupported target feature is actionable
- **WHEN** a VFX graph requires a data interface unavailable on the selected target
- **THEN** validation SHALL return a stable capability diagnostic at the responsible node or pin

#### Scenario: GPU lowering is inspectable
- **WHEN** a VFX graph compiles successfully
- **THEN** the result SHALL report its attribute layout, kernels, generated source, dependencies and estimated cost
