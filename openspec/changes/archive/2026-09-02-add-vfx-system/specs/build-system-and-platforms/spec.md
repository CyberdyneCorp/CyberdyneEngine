## MODIFIED Requirements

### Requirement: Feature options
The build SHALL expose feature options, each defining a guard macro:

`CY_BUILD_EDITOR`, `CY_BUILD_TESTS`, `CY_BUILD_TOOLS`, `CY_SCRIPTING`, `CY_PHYSICS`,
`CY_NAVIGATION`, `CY_AUDIO`, `CY_AUDIO_STEAM_AUDIO`, `CY_UI`, `CY_VFX`, `CY_NETWORKING`, `CY_XR`,
`CY_PROFILING`, `CY_RENDERER_VULKAN`, `CY_RENDERER_METAL`, `CY_RENDERER_D3D12`, `CY_SANITIZE`.

Per-module options (`CY_MODULE_<NAME>`) SHALL enable or disable each module.

Disabling a feature SHALL exclude its sources and its third-party dependencies from the build, not
merely stub it at runtime.

Options that gate an optional backend of an enabled subsystem — `CY_AUDIO_STEAM_AUDIO` gating
spatial acoustics within `CY_AUDIO` — SHALL leave the subsystem fully functional when disabled,
falling back to the engine's own implementation.

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
