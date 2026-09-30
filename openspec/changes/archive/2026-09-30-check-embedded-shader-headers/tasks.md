# Tasks

## 1. Embedded shader headers

- [x] 1.1 Declare every committed embedded-shader header in `tools/shaders/embedded_headers.toml`, with exclusions and their reasons.
- [x] 1.2 Add `tools/shaders/embedded_headers.py` (`check`, `regenerate`) and its negative cases in `tools/shaders/tests/test_embedded_headers.py`.
- [x] 1.3 Prove the check red on main's stale particle and strip headers.
- [x] 1.4 Make the embed scripts write the licence line the committed headers carry; add `--title` to the VFX MSL embedder.
- [x] 1.5 Regenerate every stale header and run the check green.
- [x] 1.6 Add `just quality-shader-headers`, the `shader-headers` gate, and the CI step with its artefact.

## 2. Clang builds

- [x] 2.1 Reproduce the clang 22 `-Wc2y-extensions` failure in the test harness and the clang 18 `-Wdouble-promotion` failures.
- [x] 2.2 Scope the suppressions to the doctest macro expansions without changing the project's warning flags.
- [x] 2.3 Build the dev profile with clang and run the unit suites.

## 3. Review

- [x] 3.1 Build and test the dev and debug profiles with GCC.
- [x] 3.2 Update `tools/shaders/README.md` and `requirements-coverage.toml`; validate this change with `--strict`.
