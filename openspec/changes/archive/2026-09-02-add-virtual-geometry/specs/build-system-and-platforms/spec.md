## MODIFIED Requirements

### Requirement: Feature options
The build SHALL expose feature options, each defining a guard macro:

`CY_BUILD_EDITOR`, `CY_BUILD_TESTS`, `CY_BUILD_TOOLS`, `CY_SCRIPTING`, `CY_PHYSICS`,
`CY_NAVIGATION`, `CY_AI`, `CY_ML`, `CY_ANIMATION`, `CY_AUDIO`, `CY_AUDIO_STEAM_AUDIO`, `CY_UI`,
`CY_VFX`, `CY_VIRTUAL_GEOMETRY`, `CY_NETWORKING`, `CY_XR`, `CY_PROFILING`, `CY_RENDERER_VULKAN`,
`CY_RENDERER_METAL`, `CY_RENDERER_D3D12`, `CY_SANITIZE`.

Per-module options (`CY_MODULE_<NAME>`) SHALL enable or disable each module.

Disabling a feature SHALL exclude its sources and its third-party dependencies from the build, not
merely stub it at runtime.

Options that gate an optional backend or acceleration path of an enabled subsystem —
`CY_AUDIO_STEAM_AUDIO` gating spatial acoustics within `CY_AUDIO`, `CY_VIRTUAL_GEOMETRY` gating the
virtualised path within the renderer — SHALL leave the subsystem fully functional when disabled,
falling back to the engine's own implementation or representation.

Features SHALL declare their dependencies on other features, and configuration SHALL fail with a
clear diagnostic when a required dependency is disabled — `CY_AI` requires `CY_NAVIGATION`.

Dependencies used only by the editor and cooker SHALL NOT be linked into a shipped runtime, and the
build SHALL enforce this separation.

#### Scenario: Minimal server build
- **WHEN** rendering, audio, UI, VFX, XR, and the editor are disabled
- **THEN** neither their sources nor their third-party dependencies SHALL be compiled or linked

#### Scenario: Optional backend disabled
- **WHEN** `CY_AUDIO` is enabled and `CY_AUDIO_STEAM_AUDIO` is disabled
- **THEN** Steam Audio SHALL not be fetched, built, or linked, and audio SHALL remain fully
  functional through the engine's fallback spatialisation

#### Scenario: VFX disabled
- **WHEN** `CY_VFX` is disabled
- **THEN** the VFX runtime, compiler, and renderers SHALL be excluded, and the rest of the
  renderer SHALL build and run unchanged

#### Scenario: AI without ML
- **WHEN** `CY_AI` is enabled and `CY_ML` is disabled
- **THEN** the AI system SHALL build and run fully, and only AI graphs containing inference nodes
  SHALL fail to cook, with a clear diagnostic

#### Scenario: Missing feature dependency
- **WHEN** `CY_AI` is enabled and `CY_NAVIGATION` is disabled
- **THEN** configuration SHALL fail naming the required dependency

#### Scenario: Tool-time dependency is not shipped
- **WHEN** a shipped runtime is built with USD import available in the editor
- **THEN** OpenUSD SHALL not be linked into the runtime, and the build SHALL fail if it is

#### Scenario: Virtual geometry disabled
- **WHEN** `CY_VIRTUAL_GEOMETRY` is disabled
- **THEN** assets SHALL render through their fallback representations with no content changes, and
  the virtualised runtime SHALL be excluded
