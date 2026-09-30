# Tasks

## 1. Harness

- [x] 1.1 `cy::test::Near`, `near` and `near_relative`; `CY_CHECK_NEAR` absolute, `CY_CHECK_NEAR_REL` relative; a failed check prints both sides.
- [x] 1.2 `unit.harness` regressions: the old expansion accepts 1000.5 against 1000.0 at 0.01 and 0.515 against 0.5 at 0.01, the new one refuses both; the bound is inclusive; NaN is refused and an exact infinity admitted; the relative form scales by the larger magnitude with no `1 +`.

## 2. Audit

- [x] 2.1 Every suite run in the Development and Debug builds; each call site the absolute tolerance turns red is classified as a defect in the code (fixed, with a regression) or a tolerance written as a fraction (`CY_CHECK_NEAR_REL`, with a comment).
- [x] 2.2 The comments that described the relative macro (`approx.h`, `test_conventions.cpp`, `test_queries.cpp`, `test_cloud_shadows.cpp`, `test_cluster.cpp`, `motion_blur/README.md`) say what it means now.
