# build-system-and-platforms Specification

## Purpose

Defines how CyberdyneEngine is built, configured, and ported: the CMake build, dependency
management, the Swift toolchain integration, code generation, build configurations, and the
platform porting surface.

## Requirements

### Requirement: CMake build system
The engine SHALL be built with **CMake 3.28 or later** and **Ninja** as the default generator,
using modern target-based CMake: every module is a target with `PUBLIC` / `PRIVATE` usage
requirements, and layering is enforced by target dependencies.

The build SHALL support presets (`CMakePresets.json`) for common configurations, so a contributor
can build with a single documented command.

#### Scenario: Layering enforced by targets
- **WHEN** a `core` target attempts to link a `scene` target
- **THEN** CMake configuration SHALL fail, because the dependency direction is declared and
  checked

#### Scenario: One-command build
- **WHEN** a contributor runs the documented preset
- **THEN** the engine SHALL configure, fetch dependencies, and build without manual steps

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

### Requirement: Dependency management
Third-party dependencies SHALL be managed through CMake `FetchContent` with pinned commit hashes,
plus a vendored fallback for dependencies requiring patches.

Every dependency SHALL be declared in a single manifest recording: name, version, commit, licence,
upstream URL, why it is used, and whether it is optional.

The build SHALL support using **system-provided** versions of dependencies where a distribution
packager requires it (`CY_USE_SYSTEM_<NAME>`).

#### Scenario: Reproducible dependency set
- **WHEN** the engine is built at a given commit
- **THEN** the exact dependency versions SHALL be determined by pinned hashes, not by "latest"

#### Scenario: Distribution packaging
- **WHEN** a packager builds with system dependencies
- **THEN** the vendored copies SHALL be excluded and the system versions linked

#### Scenario: Licence audit
- **WHEN** the licence report is generated
- **THEN** it SHALL enumerate every linked dependency with its licence, so shipping obligations are
  known

### Requirement: Swift toolchain integration
The build SHALL integrate Swift as a **consumer** of the engine, not a build dependency of the
core:

- The engine's C ABI headers SHALL be generated as a build artefact
- The `CyberdyneKit` Swift package SHALL be buildable with `swift build` against those headers
- Game Swift packages SHALL build to dynamic libraries (development) or static archives
  (shipping) and be loaded or linked by the runtime
- The Swift toolchain version SHALL be pinned per engine release and verified in CI

Building the engine SHALL NOT require a Swift toolchain unless Swift support is enabled.

#### Scenario: Engine builds without Swift
- **WHEN** `CY_SCRIPTING=OFF`
- **THEN** the build SHALL succeed on a machine with no Swift toolchain installed

#### Scenario: Overlay regeneration
- **WHEN** the ABI changes
- **THEN** the overlay generation step SHALL run and CI SHALL verify the committed overlay matches

### Requirement: Code generation
The build SHALL generate, as explicit build steps with declared inputs and outputs:

- **reflection metadata, serialization code, component registration, editor metadata, replication
  schema inputs, and binding descriptors** — all from annotated declarations, by the reflection
  generator (see `core-type-system`), which SHALL parse C++ with a real compiler frontend rather
  than a bespoke text scanner
- the **identity manifest** updates for newly declared types and fields, which SHALL be written to
  the source tree and committed, not to the build directory
- the C ABI headers and the machine-readable ABI description, from the ABI definition
- the Swift overlay, from the ABI description
- shader artefacts, from Slang sources
- the module registration table, from enabled modules
- version and build metadata (version, git hash, build configuration, timestamp)
- the default theme and editor icon atlases

Generated files SHALL be written to the build directory, except the Swift overlay and the identity
manifest, which SHALL be committed — the overlay so consumers need no generator, the manifest
because it is the authoritative record of persistent identity and its changes must be reviewable.

Generation SHALL be **deterministic**: identical inputs SHALL produce byte-identical outputs, so
generated artefacts do not churn in builds or defeat caches.

Continuous integration SHALL fail when the committed identity manifest would change in a way that
alters an existing identifier (see `core-type-system`).

#### Scenario: Incremental correctness
- **WHEN** an input to a generator changes
- **THEN** only the affected generated files and their dependents SHALL rebuild

#### Scenario: Generation does not churn
- **WHEN** the build runs twice without source changes
- **THEN** generated outputs SHALL be byte-identical and no rebuild SHALL be triggered

#### Scenario: Identity changes are gated
- **WHEN** a change would alter an existing identifier in the manifest
- **THEN** continuous integration SHALL fail, naming the entry

### Requirement: Compiler support
The engine SHALL support: **Clang 17+**, **GCC 13+**, and **MSVC 19.38+** (Visual Studio 2022
17.8), each with full C++20 support for the features the engine uses.

The build SHALL enable a strict warning set and treat warnings as errors in CI, with a documented,
narrowly-scoped suppression mechanism for third-party headers.

#### Scenario: Warning as error
- **WHEN** a change introduces a warning in engine code
- **THEN** CI SHALL fail

#### Scenario: Third-party warnings suppressed
- **WHEN** a dependency's headers produce warnings
- **THEN** they SHALL be included as system headers so they do not fail the build

### Requirement: Build performance
The build SHALL be structured for fast iteration: precompiled headers for stable core headers,
unity builds as an option for clean builds, `ccache`/`sccache` support, and modules sized so a
typical change rebuilds a bounded set of translation units.

CI SHALL track and report build times so regressions are visible.

#### Scenario: Single-file change
- **WHEN** one implementation file changes
- **THEN** only it and its dependents SHALL rebuild, and link time SHALL dominate

#### Scenario: Unity build for CI
- **WHEN** CI performs a clean build
- **THEN** the unity build option MAY be used to reduce total build time, while incremental
  developer builds keep normal granularity

### Requirement: Platform porting surface
A platform port SHALL implement: `Platform` (process, filesystem, time, dynamic libraries, crash
handling), `DisplayServer` (windows, screens, cursor, clipboard, dialogs), an input backend, an
audio backend, a graphics surface provider for each enabled RHI backend, and packaging and
deployment support.

No changes SHALL be required in `src/core/`, `src/ecs/`, `src/servers/`, or `src/scene/` to add a
platform.

Supported platforms in the initial milestone: **Linux**, **Windows**, **macOS**, each on x86-64
and ARM64.

Planned: **iOS**, **Android**, **visionOS**, **Web**. Consoles are out of scope.

#### Scenario: New platform
- **WHEN** a contributor adds a platform
- **THEN** they SHALL implement only the platform layer, and the layering check SHALL confirm no
  upper-layer changes were needed

#### Scenario: Platform-specific code is isolated
- **WHEN** platform-specific code is required
- **THEN** it SHALL live under `platform/<name>/`, not behind `#ifdef` in shared files

### Requirement: Cross-compilation and toolchains
The build SHALL support cross-compilation through CMake toolchain files, with a documented
toolchain per target, and SHALL not require the host and target to match.

#### Scenario: Cross-compiling for ARM64 Linux
- **WHEN** a toolchain file for ARM64 Linux is supplied
- **THEN** the engine SHALL cross-compile from an x86-64 host without source changes

### Requirement: Distribution artefacts
The build SHALL produce: the **editor** application, **runtime libraries** for embedding, the
**C ABI headers** and ABI description, the **`CyberdyneKit` Swift package**, **runtime templates**
per platform and configuration used when packaging a game, and the **tools** — the build service and
its command-line clients, the cooker, the packager, and the shader compiler.

The build service SHALL be the execution engine behind those command-line tools, so that a
command-line build and an editor build share one dependency graph and one derived data cache (see
`build-and-packaging`).

Artefact names SHALL encode platform, architecture, configuration, and version, and every produced
build SHALL carry the provenance record defined in `build-and-packaging`.

#### Scenario: Packaging a game
- **WHEN** a game is packaged for a platform
- **THEN** the matching runtime template SHALL be combined with the cooked content packages and
  the game's Swift module

#### Scenario: Embedding the engine
- **WHEN** an application embeds the engine as a library
- **THEN** it SHALL drive the runtime through the documented entry points rather than owning
  `main()`

#### Scenario: One execution path
- **WHEN** the same build is produced from the editor and from the command line
- **THEN** both SHALL drive the same service, graph, and cache, and produce identical artefacts

### Requirement: Continuous integration
CI SHALL, on every pull request: build all supported platforms in `Development` and `Shipping`,
run the test suites, run static analysis and formatting checks, verify the ABI baseline, verify
generated code is current, and produce a licence report.

A nightly job SHALL additionally run sanitiser builds, longer test suites, and performance
benchmarks with regression detection.

#### Scenario: Cross-platform break is caught
- **WHEN** a change compiles on Linux but not macOS
- **THEN** the pull request SHALL fail before merge

#### Scenario: Performance regression
- **WHEN** a nightly benchmark regresses beyond a threshold
- **THEN** it SHALL be reported with the commit range, so the cause can be bisected
