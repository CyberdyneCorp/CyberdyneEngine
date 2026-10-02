# Tasks

## 1. The C++ diagnostics tests as `cy_add_test` suites

- [x] 1.1 Add `CY_CHECK_MESSAGE` and `CY_REQUIRE_MESSAGE` to `cy/test/test.h`
- [x] 1.2 Convert `test_trace`, `test_loss`, `test_log`, `test_capture`, `test_privacy`, `test_source_privacy`, `test_bridge` and `test_reproduction` to `CY_TEST_CASE`, one case per behaviour, keeping every check, and declare each as `integration.diagnostics_<subject>` from a deferred `cy_add_test`
- [x] 1.3 Delete `harness.h`; keep `cy_diag_crash_probe` where `m9:crash-artefact-paths` runs it
- [x] 1.4 Add the allocation-counting emission case, the all-channel flood case and the by-field ability query case
- [x] 1.5 Replace the plain `diagnostics.overhead` run with `integration.diagnostics_overhead`, and add `just diagnose-overhead`
- [x] 1.6 Prove each new case red by a mutation of the code it holds, and restore

## 2. The Python checks behind a gate, and the determinism lint proven

- [x] 2.1 Declare the `diagnostics-privacy` gate and run its commands in the quality job
- [x] 2.2 Name the system in the determinism lint's findings, with a selftest fixture that fails when it is missing
- [x] 2.3 Declare the mutations for `m9:determinism-lint` and `m9:determinism-lint-selftest` and record their proofs with `just roadmap-falsify prove m9 --only determinism-lint --build-dir <dir> --mutate-the-tree --record`

## 3. The coverage map and the documentation

- [x] 3.1 Cite the new suites, the gate and `m9:determinism-lint-selftest` in `requirements-coverage.toml`; narrow the exemptions that stay to what is still missing
- [x] 3.2 Update `src/core/diagnostics/README.md` and the comments in `src/core/diagnostics/tests/CMakeLists.txt`

## Verification

Each mutation was applied to the subject, the suite rebuilt and run against `build/dev`, and the
source restored:

| Mutation | Suite | Red case |
|---|---|---|
| `new char[64]` on `trace_emit()`'s path | `diagnostics_trace` | emission allocates nothing |
| drop `record_loss(... BufferPressure ...)` in `drain_slot()` | `diagnostics_loss` | both pressure cases |
| `log_emit()` passes one field fewer | `diagnostics_log` | the by-field query, and the peer query |
| `trace_was_open = false` in `measure_emission_cost()` | `diagnostics_overhead` | the overhead case |
| skip the `is_absolute()` branch of `sanitise_source_path()` | `diagnostics_source_privacy` | basename and build-path cases |
| every frame under budget in `rolling_note_frame()` | `diagnostics_capture` | the frame-over-budget case |
| the writer admits every classification | `diagnostics_privacy` | the upload-policy case |
| drop the `fidelity_reason` refusal | `diagnostics_reproduction` | the fidelity case |
| a bridged warning becomes `Info` | `diagnostics_bridge` | the bridge case |
| `TraceWriter::open()` without the reset (the code before this change) | `diagnostics_loss` | the flood case and the second-trace case |
