# Proposal

## Why

Issue #90. The diagnostics module's C++ tests under `src/core/diagnostics/tests/` were executables
with a three-macro harness and a `main()` each, registered with plain `add_test()`, because they
were written before `cy/test/test.h` existed. `tools/roadmap/requirements.py` resolves `test:`
evidence only against suites `cy_add_test` declares, by case name, so fifteen
`diagnostics-profiling-and-crash` requirements were `exempt:m11e` while most of the code behind them
was built and tested. The Python checks — the crash probe, the field-macro compile check and the
source-location lint — ran inside `just test-all` and in no declared gate, although the
specification says the build-path check "SHALL be a gate rather than a review".

The determinism lint had the same shape one row over: `m9:determinism-lint` and
`m9:determinism-lint-selftest` were recorded as "not provable here", so `simulation-and-determinism`
"Determinism lint" could not cite either, and the lint's findings named a file and a line but not
the system the requirement asks for.

`src/core/diagnostics/tests/CMakeLists.txt` also promised the real overhead numbers from
`just diagnose-overhead`, a recipe that did not exist.

## What Changes

- The eight executables become nine `cy_add_test(... KIND integration)` suites,
  `integration.diagnostics_<subject>`, one case per behaviour; every check they made is kept, and
  `harness.h` is deleted. The crash probe stays at `src/core/diagnostics/tests/cy_diag_crash_probe`,
  which is where `m9:crash-artefact-paths` runs it.
- New cases the coverage notes asked for: emission allocates nothing (the trace suite counts a
  thread's allocations through the binary's replaced global allocation functions), a flood on every
  channel keeps breadcrumbs, ticks and task lifecycle and accounts for every drop, a query that
  selects one participant's rejected ability activations by typed field, and the engine's report of
  its own emission cost, which replaces the plain `diagnostics.overhead` run.
- `cy/test/test.h` gains `CY_CHECK_MESSAGE` and `CY_REQUIRE_MESSAGE`, so a moved check keeps the
  sentence that said what it meant.
- A `diagnostics-privacy` permanent gate in `tools/roadmap/gates.toml`, run by the quality job:
  the two privacy suites, `diagnostics.field_macro`, the source-location lint with its self-test,
  and `diagnostics.crash`.
- The determinism lint's findings name the system — the target whose declared profile the source
  was checked under — and its selftest fails when a finding does not. `m9:determinism-lint` and
  `m9:determinism-lint-selftest` declare the mutations that turn them red and are proven against a
  built tree.
- `just diagnose-overhead` runs the benchmark the module's README takes its table from.
- `tools/roadmap/requirements-coverage.toml` cites the new suites, the gate and the proven selftest
  criterion; the exemptions that stay name only what is still missing.

## Capabilities

### Modified Capabilities

- `simulation-and-determinism`: a lint finding names its system as well as its file and line.

## Impact

Test registration, a gate, a recipe and one lint's output format. No engine behaviour changes.
Stages 2 (a stripped Shipping crash probe symbolicated against archived symbols) and 3 (overhead
thresholds and a per-profile assertion table) of issue #90 are not in this change.
