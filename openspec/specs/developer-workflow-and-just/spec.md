# developer-workflow-and-just Specification

## Purpose

Defines the **developer workflow**: one `justfile` at the repository root as the single discoverable
entry point for building, running, testing, generating, cooking, diagnosing and releasing.

`just` **orchestrates and does not build**. CMake and Ninja own the engine, Cargo owns the editor,
the Slang toolchain owns shaders, and the engine's own tools own content; a recipe that reimplements
incremental logic is a defect. What `just` adds is consistency: four named profiles that mean the
same thing in all four toolchains, a `doctor` recipe that diagnoses the environment instead of
letting a nested tool fail obscurely, graduated test sets with stated durations, and a continuous
integration system that invokes the same recipes a developer does — so a failing check reproduces
locally by construction.

## Requirements

### Requirement: One entry point for developer tasks
The repository SHALL provide a **single discoverable entry point for developer tasks** — a `justfile`
at the repository root — so that building, testing, running, formatting, and generating code are
invoked the same way by every contributor and by continuous integration.

`just` with no arguments SHALL list the available recipes with descriptions.

A task that a contributor is expected to perform SHALL have a recipe; instructions that exist only in
documentation SHALL be treated as a gap.

#### Scenario: A new contributor builds without reading a wiki
- **WHEN** a contributor clones the repository
- **THEN** `just` SHALL list the recipes, and a documented first build SHALL be one command

### Requirement: just orchestrates and does not build
`just` SHALL be an **orchestration layer only**. The build systems remain authoritative:

| Toolchain | Owns |
|---|---|
| CMake and Ninja | The C++20 engine, its targets, and its dependencies |
| Cargo | The Rust editor workspace |
| The Slang toolchain | Shader compilation |
| The engine's own tools | Asset cooking, packaging, and patching |

A recipe SHALL NOT reimplement dependency tracking, compilation ordering, or incremental logic that a
build system already provides.

Building through the underlying build system directly SHALL remain supported and SHALL produce the
same result as the corresponding recipe.

#### Scenario: The build system stays authoritative
- **WHEN** a developer runs the underlying build tool directly
- **THEN** the result SHALL match what the recipe produces

#### Scenario: No shadow build logic
- **WHEN** a recipe needs incremental behaviour
- **THEN** it SHALL delegate to the build system rather than tracking timestamps itself

### Requirement: Recipe surface
The recipe set SHALL cover, at minimum, the following task categories, with consistent naming:

| Category | Covers |
|---|---|
| Environment | Dependency check and diagnosis, toolchain setup, first-time bootstrap |
| Build | Engine, editor, tools, shaders, everything; per-profile and per-platform |
| Run | Editor, runtime host, samples, headless runtime |
| Test | Unit, integration, editor headless, determinism, golden-image, performance |
| Quality | Format, lint, static analysis, ABI check, identity-manifest check, spec validation |
| Generate | ABI bindings, the Swift overlay, the Rust SDK, reflection data, documentation |
| Content | Import, cook, package, patch, validate content |
| Diagnose | Profile capture, trace inspection, crash artefact inspection, log collection |
| Maintenance | Clean, cache management, dependency update, version bump |
| Release | Version, changelog, artefacts, publication |

Recipe names SHALL be predictable and consistent — the same verb SHALL mean the same thing across
categories — and every recipe SHALL carry a one-line description.

#### Scenario: A task is findable by guessing
- **WHEN** a developer guesses a recipe name from the naming pattern
- **THEN** the guess SHALL usually be correct, and `just` SHALL list the alternatives when it is not

### Requirement: Build profiles are consistent across toolchains
The workflow SHALL define named build profiles that mean the same thing in **every** toolchain:

| Profile | Meaning |
|---|---|
| `debug` | Full checks, assertions, validation layers, debug information, no optimisation constraints |
| `dev` | Optimised with assertions and debug information; the default working profile |
| `profile` | Shipping optimisation with instrumentation and symbols retained |
| `release` | Shipping optimisation, checks disabled, symbols stripped to a separate artefact |

A profile SHALL select the corresponding configuration in CMake, Cargo, the shader compiler, and the
content pipeline **together**; a mismatch between them SHALL NOT be possible through the recipes.

Mixing profiles across components SHALL require an explicit override and SHALL be reported when it
occurs.

#### Scenario: Profiles cannot drift
- **WHEN** a developer builds the `profile` configuration
- **THEN** engine, editor, shaders, and content SHALL all use it

#### Scenario: A mixed build is announced
- **WHEN** components are deliberately built with different profiles
- **THEN** the resulting artefact SHALL record it and the workflow SHALL report it

### Requirement: Environment diagnosis
The workflow SHALL provide a **`doctor` recipe** that checks the development environment and reports
what is missing or incompatible: compiler and standard library, CMake, Ninja, the Rust toolchain
version, the Swift toolchain, the shader compiler, graphics validation layers, platform software
development kits, and required tools.

The report SHALL state, for each item, whether it is present, its version, whether that version is
supported, and **how to install or correct it**.

A failure caused by a missing or wrong-version tool SHALL be diagnosed by `doctor` rather than
surfacing as a build error from a nested tool.

#### Scenario: A wrong toolchain is named
- **WHEN** the installed Rust toolchain is older than required
- **THEN** `doctor` SHALL report the found version, the required version, and the correction

### Requirement: Reproducible environments
The workflow SHALL **pin the versions of the toolchains it depends on** and SHALL detect when the
local environment does not match: compiler, Rust toolchain, Swift toolchain, shader compiler, and
build tools.

Third-party dependency acquisition SHALL be reproducible, consistent with `thirdparty-dependencies`,
and a recipe SHALL be able to produce an identical dependency set from a clean checkout.

The workflow SHALL support containerised or otherwise isolated environments for reproducing a build
exactly, and continuous integration SHALL use the same pinned versions as developers.

#### Scenario: A clean checkout builds identically
- **WHEN** a build is performed from a clean checkout with pinned versions
- **THEN** it SHALL produce the same artefacts as continuous integration

### Requirement: Continuous integration uses the same recipes
Continuous integration SHALL invoke the **same recipes** developers invoke, rather than maintaining a
parallel script set.

A check that gates a change SHALL be runnable locally by one command, and the local and remote
invocations SHALL be the same code path.

Where continuous integration needs additional behaviour — artefact upload, caching, sharding — it
SHALL wrap the recipes rather than replace them.

#### Scenario: A failing check reproduces locally
- **WHEN** a continuous integration check fails
- **THEN** the developer SHALL be able to reproduce it with the same recipe locally

### Requirement: Cross-platform and cross-compilation
Recipes SHALL work on the supported development hosts, and SHALL avoid host-specific shell
constructs that silently behave differently across platforms.

The workflow SHALL support **selecting a target platform** for build, test, deploy, and package
recipes, and SHALL report clearly when a target cannot be built on the current host and why.

Deployment to a device or console SHALL be a recipe, and SHALL integrate with the hosted-runtime and
remote play modes rather than being a separate manual procedure.

#### Scenario: A remote target is one command
- **WHEN** a developer deploys a build to a device
- **THEN** it SHALL be a recipe that produces a runtime the editor can host remotely

#### Scenario: An impossible target is explained
- **WHEN** a target cannot be built on the current host
- **THEN** the workflow SHALL say so and state what is required

### Requirement: Code generation is reproducible and checked
Generated artefacts — the C ABI description, the Swift overlay, the Rust SDK, reflection data, and
generated documentation — SHALL be produced by recipes, SHALL be **deterministic** for a given input,
and SHALL be verifiable.

A recipe SHALL be able to assert that committed generated output matches what regeneration produces,
and continuous integration SHALL run that assertion.

A stale generated artefact SHALL be a build or check failure, not a runtime surprise.

#### Scenario: Stale bindings fail the build
- **WHEN** the ABI changes without regenerating the bindings
- **THEN** the check SHALL fail and name the drift

### Requirement: Test recipes are graduated
Test recipes SHALL be graduated by cost so that the fast set is genuinely fast:

| Recipe class | Contains | Expectation |
|---|---|---|
| Fast | Unit tests and headless editor tests | Runs on every save-and-check cycle |
| Standard | Fast plus integration and content tests | Runs before pushing |
| Full | Standard plus golden-image, determinism, performance, and platform tests | Runs in continuous integration |

Each class SHALL be a single recipe, and each SHALL state its expected duration.

Test selection by name, by capability, and by changed area SHALL be supported.

#### Scenario: The fast set stays fast
- **WHEN** the fast test recipe exceeds its stated duration
- **THEN** it SHALL be treated as a regression to correct rather than a new normal

### Requirement: Diagnostics are one command away
Capturing a profile, inspecting a trace, symbolicating and inspecting a crash artefact, and
collecting logs SHALL each be a recipe.

A developer reporting a defect SHALL be able to produce a **complete reproduction bundle** with one
command — build identity, configuration, logs, trace, crash artefact, and where applicable the
captured viewport view state and replay.

The bundle format SHALL be the one defined by `diagnostics-profiling-and-crash`.

#### Scenario: A report is complete by default
- **WHEN** a developer reports a defect using the recipe
- **THEN** the bundle SHALL contain everything needed to reproduce it

### Requirement: Recipes are honest about their effects
A recipe that deletes, overwrites, rewrites history, publishes, or otherwise performs an irreversible
action SHALL **state what it will do and require confirmation**, unless explicitly invoked in a
non-interactive mode.

A destructive recipe SHALL name exactly what it will remove, and clean recipes SHALL be scoped —
cleaning one component SHALL NOT require rebuilding everything.

A recipe SHALL fail loudly on error rather than continuing with a partial result, and SHALL not mask
the exit status of the tool it invokes.

#### Scenario: A clean says what it removes
- **WHEN** a destructive recipe is invoked interactively
- **THEN** it SHALL name what will be removed and require confirmation

#### Scenario: Failures propagate
- **WHEN** an invoked tool fails
- **THEN** the recipe SHALL fail with the same status and SHALL not continue

### Requirement: Local overrides without editing the shared workflow
The workflow SHALL support **local configuration** — build directories, toolchain paths, target
device, parallelism, cache locations — through an ignored local settings file and environment
variables, so that developers do not modify shared files to work locally.

Local overrides SHALL be reported when they are in effect, so that a divergence from the default
configuration is visible when diagnosing a problem.

#### Scenario: A local override is visible
- **WHEN** a developer overrides the build directory locally
- **THEN** recipes SHALL report that an override is active

### Requirement: The workflow is documented by itself
Recipe descriptions SHALL be the primary documentation of the developer workflow, and prose
documentation SHALL reference recipes rather than restating their commands.

Where a document describes a procedure that has a recipe, it SHALL name the recipe; a documented
sequence of raw commands with no recipe SHALL be treated as a missing recipe.

#### Scenario: Documentation cannot drift from commands
- **WHEN** documentation describes how to run the tests
- **THEN** it SHALL name the recipe rather than restating a command line

### Requirement: Forbidden workflow patterns
The following SHALL NOT appear, and each SHALL be checkable:

- A recipe reimplementing incremental build logic or dependency tracking
- A continuous integration script that duplicates rather than invokes recipes
- A profile that means different things to different toolchains
- A destructive recipe that runs without confirmation in interactive use
- A recipe that swallows the exit status of the tool it invokes
- A required developer task documented only as prose with no recipe
- A generated artefact committed without a check that regeneration reproduces it
- Contributors editing shared workflow files to configure local paths

#### Scenario: A proposal is checked
- **WHEN** a change adds a continuous integration step that duplicates a recipe's logic
- **THEN** it SHALL be flagged against this requirement
