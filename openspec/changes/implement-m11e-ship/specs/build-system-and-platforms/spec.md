## MODIFIED Requirements

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

**The "Planned" list SHALL be the build system's own statement of what it supports, and it SHALL be
true.** `CY_MODULE_PLATFORMS` names seven platforms and the configure-time diagnostic names three
supported and four planned; a module manifest may therefore declare a platform for which nothing
builds. At **1.0** each planned platform SHALL be in one of exactly two states: **delivered**, with a
port that satisfies this requirement and an artefact that was produced; or **deferred**, recorded
with what is unmet, why, and the condition that brings it back, per `delivery-roadmap`. A platform
that is neither SHALL fail the check.

**At least one non-desktop platform SHALL be delivered before 1.0, or its deferral SHALL be
recorded.** The porting surface's claim is that no upper-layer change is required to add a platform,
and every implementation of it to date has been a desktop sharing a windowing model, a filesystem
model and a process model with the host that wrote it. An abstraction validated only against
platforms of one kind is validated against that kind, and this is the requirement that says so.

#### Scenario: New platform
- **WHEN** a contributor adds a platform
- **THEN** they SHALL implement only the platform layer, and the layering check SHALL confirm no
  upper-layer changes were needed

#### Scenario: Platform-specific code is isolated
- **WHEN** platform-specific code is required
- **THEN** it SHALL live under `platform/<name>/`, not behind `#ifdef` in shared files

#### Scenario: A planned platform is neither delivered nor deferred
- **WHEN** the 1.0 record is evaluated and a platform named in `CY_MODULE_PLATFORMS` has no port and
  no recorded deferral
- **THEN** the check SHALL fail naming that platform, rather than the planned list standing as a
  statement of intent nobody is accountable for

#### Scenario: The porting surface meets something that is not a desktop
- **WHEN** a non-desktop platform is ported
- **THEN** it SHALL require no change in `src/core/`, `src/ecs/`, `src/servers/` or `src/scene/`, and
  any change it does require SHALL be recorded as a defect in the abstraction rather than absorbed

### Requirement: Cross-compilation and toolchains
The build SHALL support cross-compilation through CMake toolchain files, with a documented
toolchain per target, and SHALL not require the host and target to match.

**A toolchain file SHALL exist for each target the build claims to cross-compile for, and the claim
SHALL be judged by a cross-compilation that ran, not by a configure step that succeeded.** A target
whose only evidence is a native build on a runner that happens to match the target is not evidence of
cross-compilation, because the property under test is precisely that the host and the target may
differ.

**A dependency that has no build for a target SHALL be named, not discovered.** Cross-compiling this
engine means cross-compiling everything the enabled feature set pulls in; where an integrated
dependency cannot build for a target, the resolution SHALL be recorded — vendored, replaced behind
its engine-owned interface, or feature-gated off for that target with the capability that removes
named.

#### Scenario: Cross-compiling for ARM64 Linux
- **WHEN** a toolchain file for ARM64 Linux is supplied
- **THEN** the engine SHALL cross-compile from an x86-64 host without source changes

#### Scenario: A claimed target has no toolchain file
- **WHEN** the build claims support for a target and `cmake/` contains no toolchain file for it
- **THEN** the check SHALL fail naming the target

#### Scenario: A dependency blocks a target
- **WHEN** an enabled dependency cannot build for a cross-compilation target
- **THEN** the build SHALL report which dependency and which target, and the resolution SHALL be
  recorded against that dependency rather than the target being quietly dropped

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

**The release recipes SHALL produce them.** A version, a changelog and a set of artefacts assembled
by hand in a shell history is the thing this recipe category was created to prevent, and a recipe
that refuses is a failing exit criterion rather than a deferral. `developer-workflow-and-just` owns
the recipes' shape; this requirement owns what they SHALL produce.

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

#### Scenario: A release is produced by the recipes
- **WHEN** a release is made
- **THEN** the version, the changelog and the artefacts SHALL each be produced by the release
  recipe that names them, and a recipe that refuses SHALL fail the check rather than being read as
  work not yet scheduled

### Requirement: Continuous integration
CI SHALL, on every pull request: build all supported platforms in `Development` and `Shipping`,
run the test suites, run static analysis and formatting checks, verify the ABI baseline, verify
generated code is current, and produce a licence report.

A nightly job SHALL additionally run sanitiser builds, longer test suites, and performance
benchmarks with regression detection.

**The matrix SHALL compare its legs, not merely run them.** Independent legs answer "does it build
and pass here"; they cannot answer "does it produce the same result there", which is the question
`simulation-and-determinism` and `procedural-content-generation` both depend on. At least one job
SHALL publish one leg's digest and compare it against another leg's, and every cross-platform claim
SHALL be judged by that comparison rather than by two green ticks.

**A cross-compilation target that is built SHALL be built on every pull request**, so that a change
breaking a port fails before merge rather than at the next release.

#### Scenario: Cross-platform break is caught
- **WHEN** a change compiles on Linux but not macOS
- **THEN** the pull request SHALL fail before merge

#### Scenario: Performance regression
- **WHEN** a nightly benchmark regresses beyond a threshold
- **THEN** it SHALL be reported with the commit range, so the cause can be bisected

#### Scenario: Two legs disagree
- **WHEN** two legs of the matrix produce different digests for a claim declared to be identical
  across platforms
- **THEN** the comparison job SHALL fail naming both legs and the digests, rather than each leg
  reporting success in isolation

#### Scenario: A claim no leg can judge
- **WHEN** a criterion's subject is hardware no runner in the matrix has
- **THEN** it SHALL report **not evaluated** with the reason, and SHALL NOT report a pass on the
  evidence of a machine that cannot judge it
