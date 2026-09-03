# Implement M0 — Ground: the toolchain, the skeleton, and the first gates

## Why

This is the first change that writes code. Its job is not to be impressive — the closing artefact
opens a window and exits — but to make every subsequent milestone cheap by putting the structure in
place before there is anything to restructure.

Three things are being bought here, and each is far more expensive later:

**Layering that is enforced rather than intended.** `engine-architecture` states the layer order and
`project-and-plugins` requires it enforced by the project graph. Enforced from the first target, a
violation is a configure error nobody argues with. Enforced at M4, it is an archaeology exercise
across four subsystems.

**One entry point.** `developer-workflow-and-just` requires that a task a contributor is expected to
perform has a recipe, and that CI invokes the same recipes. Established on day one, it is simply how
the project works. Established after four milestones of accumulated shell commands, it is a
migration nobody schedules.

**Diagnostics with privacy classification from the first field.** `diagnostics-profiling-and-crash`
requires every diagnostic field to carry a privacy classification. Unclassified fields accumulate
faster than they can be audited — this is one of the invariants the roadmap pins to M0 for exactly
that reason.

Everything else in M0 exists to make those three demonstrable: something has to open a window for a
`DisplayServer` to be worth having, something has to run for a trace to contain anything, and a test
harness has to exist before any gate can be green.

## What Changes

Four workstreams, one milestone gate. See `tasks.md` for the ordered plan and `design.md` for the
decisions behind it.

- **Repository skeleton and build.** The directory layout `engine-architecture` specifies, CMake
  3.28 with Ninja and presets, the four build configurations, the full `CY_*` feature option set
  with generated headers, the module system under `modules/`, pinned dependency management with a
  manifest and generated attribution, and **layer enforcement as a configure-time failure**.
- **Developer workflow.** The root `justfile` covering every recipe category the specification
  names, `just doctor` diagnosing the environment rather than letting a nested tool fail obscurely,
  four profiles that mean the same thing across toolchains, local overrides that announce
  themselves, and CI on three platforms that invokes recipes and nothing else.
- **Platform and diagnostics.** `Platform` and `DisplayServer` as engine-owned interfaces, an
  **SDL3-backed desktop implementation** and a **headless implementation** behind them, a runtime
  exposing `tick()` rather than owning a loop, `cy::Expected`, the assertion macros, structured
  logging, the single trace timeline with its buffering and loss policy, **privacy classification on
  every field**, and a crash handler that writes a report.
- **Tests, gates and roadmap tooling.** The test taxonomy's directories and budgets with **doctest**
  as the framework, a smoke test that runs the sample, formatting and static analysis, sanitizer
  builds, and the roadmap recipes deferred to M0 by `add-delivery-roadmap` — `roadmap::status` and
  `roadmap::milestone`.

**Closing artefact**: `samples/00-empty` — opens a window, runs an empty loop, writes a trace, exits
cleanly. Run by `just run::sample empty` and by the smoke test in CI.

**Exit criteria** are the M0 row of the roadmap, executable as `just roadmap::milestone m0`.

## Capabilities

### Modified Capabilities

- `thirdparty-dependencies` — record the two dependency decisions this change makes: **SDL3** covers
  windowing and window events as well as input and gamepads, with native per-platform backends
  planned rather than assumed; **doctest** is the test framework, chosen for compile time.
- `delivery-roadmap` — correct the platform ladder. It said "console porting surface", which
  contradicts `core-platform-abstraction`, where consoles are explicitly out of scope and iOS,
  Android, visionOS and Web are the planned targets. Add the native `DisplayServer` backends to M11
  so the abstraction is validated against a second implementation before 1.0.

### Advanced Capabilities

No capability reaches Working here. Eight reach **Seed**: `build-system-and-platforms`,
`developer-workflow-and-just`, `thirdparty-dependencies`, `testing-and-quality`,
`core-platform-abstraction`, `diagnostics-profiling-and-crash`, `project-and-plugins`, and
`delivery-roadmap` at **Working** for its tooling. `docs/roadmap/status.yaml` is updated as each
lands.

## Impact

- **New code**: `src/core/`, `platform/`, `src/runtime/`, `tools/`, `tests/`, `benchmarks/`,
  `samples/`, `cmake/`, `modules/`, the root `justfile` and `CMakePresets.json`.
- **New dependencies**: SDL3, doctest, Tracy (behind `CY_PROFILING`), zstd and BLAKE3 (the trace
  writer and the derivation-key groundwork), each pinned in the manifest and each behind an
  engine-owned interface.
- **New permanent gates**: three-platform build and test, format, static analysis, the layering
  check, generated-code-currency, spec validation, and `roadmap::status`. Once green they stay green
  — a later milestone may not break them.
- **Risk**: the profile mapping across toolchains, spiked first. Everything else in M0 is
  well-specified work whose main hazard is doing more of it than the milestone needs.
