## MODIFIED Requirements

### Requirement: Build configurations
The build SHALL define these configurations:

| Configuration | Optimisation | Assertions | Editor | Use |
|---|---|---|---|---|
| `Debug` | None | On | On | Debugging the engine |
| `Development` | On | On | On | Day-to-day development; the default |
| `Profile` | On | Off | Off | Profiling with symbols and instrumentation |
| `Shipping` | Full, LTO | Off | Off | Release builds |

`CY_DEVELOPMENT` SHALL be defined in `Debug` and `Development`, gating assertions, diagnostics,
hot reload, and debug visualisation.

A **dedicated server** build SHALL be selectable orthogonally to configuration, defining
`CY_DEDICATED_SERVER` and excluding the renderer, VFX, UI, client audio, and the editor at link
time rather than disabling them at runtime.

A dedicated server build SHALL pair with the `DedicatedServer` cook profile (see
`asset-import-pipeline`), and the build SHALL fail if a client-only subsystem is linked into it.

#### Scenario: Shipping strips development code
- **WHEN** a `Shipping` build is produced
- **THEN** assertions, debug draw, editor code, and hot reload SHALL be absent from the binary

#### Scenario: Profile keeps symbols
- **WHEN** a `Profile` build runs under a profiler
- **THEN** symbols and instrumentation SHALL be present while assertions are off, so measurements
  reflect shipping performance

#### Scenario: Server links no graphics
- **WHEN** a dedicated server build is produced
- **THEN** no graphics backend, VFX runtime, or UI runtime SHALL be linked, and the build SHALL
  fail if one is

#### Scenario: Server build and cook agree
- **WHEN** a dedicated server is packaged
- **THEN** the `DedicatedServer` cook profile SHALL be used, so the binary and its content match
