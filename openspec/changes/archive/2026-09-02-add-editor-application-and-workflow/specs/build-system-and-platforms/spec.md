## ADDED Requirements

### Requirement: Rust toolchain integration
The build system SHALL integrate the **Rust toolchain** for the editor application and its
supporting tools, alongside the C++ and Swift toolchains.

The Rust toolchain version SHALL be **pinned** in the repository, and a mismatch SHALL be reported by
the environment diagnosis defined in `developer-workflow-and-just` rather than surfacing as a Cargo
error.

Cargo SHALL own compilation of the Rust workspace; the C++ build SHALL NOT reimplement it. Ordering
between the two SHALL be explicit: the engine's shared library and its generated ABI description are
inputs to the Rust build.

Build configurations SHALL map consistently onto Cargo profiles, so that the engine's `debug`, `dev`,
`profile`, and `release` configurations select the corresponding Rust profile.

The Rust workspace SHALL be buildable independently for editor-only work, using a prebuilt engine
library, so that interface iteration does not require a full engine build.

Cross-compilation targets for the editor SHALL be declared alongside the engine's, and a target
unsupported by one toolchain SHALL be reported rather than producing a partial artefact.

#### Scenario: A version mismatch is diagnosed, not raw
- **WHEN** the installed Rust toolchain differs from the pinned version
- **THEN** the environment check SHALL report it with the required version and the correction

#### Scenario: Editor iteration does not rebuild the engine
- **WHEN** only editor code changes
- **THEN** the build SHALL compile the Rust workspace against the existing engine library

#### Scenario: Profiles map across toolchains
- **WHEN** the `profile` configuration is selected
- **THEN** the C++, Rust, shader, and content builds SHALL all use it
