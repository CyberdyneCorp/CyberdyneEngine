# Tasks: M0 — Ground

The implementation plan. Ordered: the spike first, then the skeleton everything else attaches to,
then the three workstreams, then the gate that closes the milestone.

Workstreams **B**, **C** and **D** depend on **A** and on nothing else, so once A.4 configures they
may proceed in any order or concurrently.

Nothing here has a date. Section 0 is the risk; sections 1 to 4 are the work; section 5 closes the
milestone.

---

## 0. Spike — profiles across toolchains

The named risk of this milestone. Its only deliverable is a decision, recorded in `design.md`.

- [x] 0.1 Write `cmake/profiles.cmake` and the `justfile` profile table with the Cargo and Slang
      columns filled in and unused, per `design.md` §7
- [x] 0.2 Prove one profile end to end: `just build-engine --profile dev` configures and builds a trivial
      target with the expected flags, assertions and definitions
- [x] 0.3 Confirm the reserved Cargo and Slang columns are expressible — a written mapping, not an
      intention — and record the result in `design.md`
- [x] 0.4 If the mapping does not survive, propose the roadmap or workflow change **before**
      section 1 proceeds

---

## 1. Workstream A — Repository skeleton and build

### 1.1 Layout

- [x] 1.1.1 Create the directory layout `engine-architecture` specifies: `src/core/`, `src/ecs/`,
      `src/servers/`, `src/backends/`, `src/scene/`, `src/runtime/`, `src/abi/`, `modules/`,
      `platform/`, `tools/`, `bindings/`, `editor/`, `tests/`, `benchmarks/`, `samples/`, `cmake/`,
      `deps/` — each with a `README.md` stating what belongs there and which capability governs it
- [x] 1.1.2 `.gitattributes`, `.editorconfig`, and the `.gitignore` additions for build trees and
      caches
- [x] 1.1.3 `CONTRIBUTING.md` pointing at `just` and the OpenSpec flow, restating no procedure that
      has a recipe

### 1.2 CMake foundation

- [x] 1.2.1 Top-level `CMakeLists.txt` requiring CMake 3.28, Ninja as the default generator, C++20,
      `-fno-exceptions` and `-fno-rtti`, warnings-as-errors, and no in-source builds
- [x] 1.2.2 `CMakePresets.json` — one preset per profile per platform, so a documented first build
      is one command
- [x] 1.2.3 The four configurations from `build-system-and-platforms`: `Debug`, `Development`,
      `Profile`, `Shipping`, with `CY_DEVELOPMENT` defined in the first two
- [x] 1.2.4 `CY_DEDICATED_SERVER` as an orthogonal option, with the link-time exclusion rule
      recorded and its check stubbed until there is a renderer to exclude
- [x] 1.2.5 Compiler support matrix in `cmake/compilers.cmake`: minimum Clang, GCC and MSVC
      versions, checked at configure time with a clear diagnostic

### 1.3 Layering enforcement — **invariant, M0**

- [x] 1.3.1 `cy_add_module(NAME <n> LAYER <layer> ...)` in `cmake/module.cmake`: records the layer
      as a target property, declares `PUBLIC`/`PRIVATE` usage requirements, and **fails at configure
      time** when a target links one at a higher layer, naming both targets and their layers
- [x] 1.3.2 Lint that fails on a bare `add_library` or `add_executable` in the engine tree — there
      is no way to declare a target that opts out of the layer check
- [x] 1.3.3 `tools/layercheck/` — source-level `#include` check catching upward includes CMake never
      saw, including the `platform/` rule that no SDL header appears above it
- [x] 1.3.4 Negative tests: a fixture project whose configure **must** fail for an upward link, an
      upward include, and an SDL include above `platform/`
- [ ] 1.3.5 Wire both checks into `just quality-layers` and into CI

### 1.4 Feature options and generated headers

- [x] 1.4.1 Declare the full `CY_*` option set from `build-system-and-platforms`, defaulting `OFF`
      where the subsystem does not exist yet
- [x] 1.4.2 Feature dependency declarations and configure-time validation with a diagnostic naming
      the required option — `CY_AI` requires `CY_NAVIGATION`
- [x] 1.4.3 Generate `cy_features.h` and `cy_modules.h`; regeneration is reproducible and checked
- [x] 1.4.4 Test that a disabled feature excludes its sources and its dependencies from the build
      rather than stubbing at runtime

### 1.5 Module system

- [x] 1.5.1 `modules/<name>/` layout with a manifest declaring name, description, layer, type,
      public and private dependencies, default-enabled, supported platforms, and hot-reload support
- [x] 1.5.2 `CY_MODULE_<NAME>` options generated from the discovered manifests, and
      `CY_EXTRA_MODULE_PATHS` so an out-of-tree module builds exactly as an in-tree one
- [x] 1.5.3 Registration levels `Core`, `Servers`, `Scene`, `Editor` — ordering initialisation,
      distinct from layer, which constrains dependencies
- [x] 1.5.4 Project-graph validation per `project-and-plugins`: undeclared dependencies and cycles
      are build errors
- [x] 1.5.5 One real module — `modules/example-null/` — proving discovery, options, ordering and
      exclusion, and serving as the template

### 1.6 Dependencies

- [x] 1.6.1 `deps/manifest.toml`: name, version, exact commit, licence, source, the engine-owned
      interface it sits behind, and the gating feature option
- [x] 1.6.2 `cmake/dependencies.cmake` driving `FetchContent` from the manifest, with pinned commits
      and no network access required for an already-populated cache
- [x] 1.6.3 Add SDL3, doctest, Tracy (behind `CY_PROFILING`), zstd, and BLAKE3
- [x] 1.6.4 `tools/deps/` generates `THIRD_PARTY.md` from the manifest; CI fails when it is stale
- [x] 1.6.5 Test that disabling a feature does not fetch, build or link its dependencies

---

## 2. Workstream B — Developer workflow

### 2.1 The entry point

- [x] 2.1.1 Root `justfile`; bare `just` lists every recipe with a one-line description
- [x] 2.1.2 Namespaced recipes covering every category the recipe surface names: `env`, `build`, `run`, `test`, `quality`, `generate`, `content`, `diagnose`, `roadmap`,
      `maintenance`, `release` — one imported file per category under `just/`, recipes named
      `<category>-<verb>` per `design.md` §10 — with `content` and `release` present and honestly reporting they are not yet
      implemented
- [x] 2.1.3 Naming consistency: the same verb means the same thing across categories
- [x] 2.1.4 Every recipe forwards the exit status of the tool it invokes — never swallows it
- [x] 2.1.5 Destructive recipes confirm in interactive use and take a flag for CI

### 2.2 `doctor`

- [x] 2.2.1 `just env-doctor` checks CMake ≥ 3.28, Ninja, a supported compiler, git, Python, and
      the OpenSpec CLI, reporting version, path and status for each
- [x] 2.2.2 Each failure names the fix for the host platform rather than reporting absence
- [x] 2.2.3 `just env-bootstrap` — first-time setup, idempotent, safe to re-run
- [x] 2.2.4 Test `doctor` against a deliberately broken environment and assert its exit code and
      the missing-tool report

### 2.3 Profiles and overrides

- [x] 2.3.1 Wire the profile table from the spike into every build, run and test recipe
- [x] 2.3.2 Local overrides through `.just.local` and environment variables, never by editing shared
      files
- [x] 2.3.3 Recipes report when an override is active

### 2.4 Continuous integration

- [ ] 2.4.1 GitHub Actions matrix: Linux, Windows and macOS, x86-64 and ARM64, invoking **only**
      `just` recipes
- [ ] 2.4.2 Jobs: build, test, quality, spec validation, generated-code currency, `roadmap-status`
- [ ] 2.4.3 Dependency and build caching keyed on the manifest
- [x] 2.4.4 A check that a workflow file does not duplicate logic a recipe already has
- [x] 2.4.5 PR template with the capability-and-tier field `delivery-roadmap` requires

---

## 3. Workstream C — Platform and diagnostics

### 3.1 Core primitives

- [x] 3.1.1 `cy::Expected<T, Error>` and the `Error` model — no exceptions cross any boundary
- [x] 3.1.2 `CY_ASSERT` family: fires in `Debug` and `Development`, compiled out of `Profile` and
      `Shipping`, with the failure routed through diagnostics
- [x] 3.1.3 The minimum type aliases M0 needs (`u8`…`f64`, `usize`) — no containers, no allocators,
      no math; those are M1

### 3.2 `Platform`

- [x] 3.2.1 The `Platform` interface: process lifetime and exit code, arguments, environment,
      standard output and error, user data / config / cache directories, executable path, dynamic
      library loading and symbol resolution, subprocess creation, monotonic and wall clocks, locale,
      CPU count and features, memory statistics, crash handler installation
- [x] 3.2.2 Monotonic clock in nanoseconds, separate from wall clock; test that adjusting the system
      clock does not affect frame timing
- [x] 3.2.3 `platform/desktop-sdl3/` implementation for Linux, Windows and macOS
- [x] 3.2.4 Platform-specific code lives under `platform/<name>/`, never behind `#ifdef` in shared
      files — checked by 1.3.3

### 3.3 `DisplayServer`

- [x] 3.3.1 The interface: window creation and destruction, position, size, min and max size, title,
      icon, mode, flags, DPI scale, screen enumeration, V-sync mode, and native surface creation
- [x] 3.3.2 `has_feature(Feature)` — capabilities are queried, never assumed; an unsupported request
      warns and is ignored rather than failing window creation
- [x] 3.3.3 SDL3-backed desktop implementation
- [x] 3.3.4 **Headless implementation** satisfying the interface with no window system — required by
      the specification and by CI
- [x] 3.3.5 Surface creation returns the platform surface for the active backend, with the seam
      exercised by a stub since there is no RHI until M3
- [x] 3.3.6 DPI-change event delivered when a window moves between screens of different scale

### 3.4 Runtime host — **invariant, M0**

- [x] 3.4.1 `Runtime::tick()` as the entry point; the runtime never owns a `while (running)` loop
- [x] 3.4.2 `src/runtime/` bootstrap: deterministic startup and shutdown ordering, with the order
      recorded and asserted
- [x] 3.4.3 Desktop host under `platform/` calls `tick()`; the loop lives in the host
- [x] 3.4.4 Test that startup and shutdown order is identical across 100 runs

### 3.5 Diagnostics — **invariant, M0**

- [x] 3.5.1 One trace with many producers: trace identity, formatting, and the timeline every
      subsystem will publish into
- [x] 3.5.2 **Privacy classification as a required argument of the field macro**, with no overload
      that omits it, per `design.md` §2
- [x] 3.5.3 Redaction applied by the writer according to classification and export policy; test that
      a field cannot be exported at a level it was not classified for
- [x] 3.5.4 Buffering and loss policy: bounded buffers, loss recorded in the trace rather than
      silently dropped
- [x] 3.5.5 Structured logging with categories, levels and the same timeline
- [x] 3.5.6 Trace written to a file the `diagnose::trace` recipe can inspect
- [x] 3.5.7 Tracy behind `CY_PROFILING`, as a backend of the engine's own trace rather than beside it
- [x] 3.5.8 Crash handler writes a report to the user mount: signal or exception, symbolised
      backtrace where available, engine version, last logged frame
- [x] 3.5.9 Diagnostics overhead measured and recorded, so the budget claim is a number

### 3.6 The closing artefact

- [x] 3.6.1 `samples/00-empty` — opens a window, runs an empty loop through `tick()`, writes a
      trace, exits cleanly
- [x] 3.6.2 `just run-sample empty` runs it; `--headless` runs it under the headless display server
- [x] 3.6.3 Clean shutdown on window close, on `SIGINT`, and on a frame count limit

---

## 4. Workstream D — Tests, gates and roadmap tooling

### 4.1 Test harness

- [x] 4.1.1 doctest integrated behind a `CY_TEST_CASE` wrapper, so the framework is replaceable
- [x] 4.1.2 The taxonomy's directories with their stated budgets: `tests/unit/<module>/`,
      `tests/integration/`, `tests/smoke/`, `benchmarks/` — and `tests/render/`,
      `tests/determinism/` created empty with a README naming the milestone that fills them
- [x] 4.1.3 Graduated recipes: `test-unit` under a minute, `test-integration`, `test-smoke`,
      `test-all`, each stating its duration
- [x] 4.1.4 Smoke test runs `samples/00-empty` headless and asserts a clean exit and a trace file
- [x] 4.1.5 Benchmark harness with regression thresholds, exercised by one trivial benchmark

### 4.2 Static analysis and sanitizers

- [x] 4.2.1 `.clang-format` and `just quality-format` and `just quality-format-check`
- [x] 4.2.2 `.clang-tidy` and `just quality-lint`, with the rule set justified rather than default
- [ ] 4.2.3 `CY_SANITIZE` wiring ASan, UBSan and TSan; a CI job running the unit set under ASan and
      UBSan
- [x] 4.2.4 `just quality-specs` runs `openspec validate --specs --strict`

### 4.3 Roadmap tooling — deferred to M0 by `add-delivery-roadmap` §4

- [x] 4.3.1 `just roadmap-status` reports every capability's tier, the milestone that last advanced
      it, and the change that did so, from `docs/roadmap/status.yaml`
- [x] 4.3.2 It **exits non-zero** when the record and `openspec/specs/` disagree — a capability
      added, renamed or removed without a record entry is drift, and drift fails the build
- [x] 4.3.3 `just roadmap-milestone <id>` runs a milestone's full exit criteria and exits non-zero
      if any fail
- [x] 4.3.4 M0's criteria expressed as that recipe's check list
- [ ] 4.3.5 `roadmap-status` runs on every pull request
- [x] 4.3.6 Record the deferred-seam checks — multi-view, runtime-driven frame timing, late latching
      — as `tests/render/README.md` entries to be wired when the renderer lands at M3

### 4.4 Merge gates

- [x] 4.4.1 Assemble the permanent gate set: three-platform build and test, format, lint, layering,
      generated-code currency, spec validation, `roadmap-status`
- [x] 4.4.2 A failing gate requires an explicit recorded override, not a quiet exception
- [x] 4.4.3 Record in `testing-and-quality`'s terms that M0's criteria join the gate set on close
      and stay green afterwards

---

## 5. Closing the milestone

- [ ] 5.1 `just doctor && just build && just test` green on Linux, Windows and macOS in CI
- [ ] 5.2 `just run-sample empty` opens and closes a window on all three
- [x] 5.3 A trace file is produced and readable by `just diagnose-trace`
- [x] 5.4 Format, lint and static analysis gates live and green
- [x] 5.5 The layering check fails on each of the three deliberately introduced violations
- [ ] 5.6 `just roadmap-milestone m0` exits zero
- [x] 5.7 Update `docs/roadmap/status.yaml`: `build-system-and-platforms`,
      `developer-workflow-and-just`, `thirdparty-dependencies`, `testing-and-quality`,
      `core-platform-abstraction`, `diagnostics-profiling-and-crash` and `project-and-plugins` to
      **seed**; `delivery-roadmap` to **working**; each naming this change
- [x] 5.8 Update `docs/roadmap/capability-matrix.md` and `docs/ROADMAP.md` where M0 recorded an
      intention that the implementation changed
- [ ] 5.9 `openspec validate --specs --strict` passes; archive this change
- [ ] 5.10 Open the M1 change — the reflection generator's incrementality is its named spike

---

## 6. Planning artefacts

The work of authoring this change. Complete; the implementation sections above are not.

- [x] 6.1 `thirdparty-dependencies` — record SDL3 as the windowing, input and gamepad backend
      beneath `DisplayServer` with native backends planned, and doctest as the test framework with
      the compile-time argument stated
- [x] 6.2 `delivery-roadmap` — correct the platform ladder, which said "console porting surface"
      against `core-platform-abstraction`, where consoles are out of scope and iOS, Android,
      visionOS and Web are the planned targets; add the native platform backend to M11
- [x] 6.3 `docs/ROADMAP.md` and `docs/roadmap/dependencies.md` updated to match, in this change
- [x] 6.4 `docs/roadmap/implementing.md` — how a milestone becomes OpenSpec changes, the rules every
      implementation change carries, and the in-flight pointer
- [x] 6.5 `design.md` records the decisions the specifications left open: layering enforcement,
      privacy classification, `tick()`, SDL3, doctest, the dependency manifest, the profile table,
      early feature options, and what M0 deliberately does not do
