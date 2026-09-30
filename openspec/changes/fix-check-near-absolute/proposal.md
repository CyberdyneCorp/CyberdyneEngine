# Proposal: CY_CHECK_NEAR's tolerance is absolute

## Why

`CY_CHECK_NEAR(value, expected, tolerance)` in `tests/harness/include/cy/test/test.h` promises "an
explicit tolerance", and every reader takes the third argument as an amount. It expanded to
`doctest::Approx(expected).epsilon(tolerance)`, which is RELATIVE: it admits
`tolerance * (1 + max(|value|, |expected|))`. A check on a value near a thousand was a thousand
times looser than written, and below one it still admitted up to twice the tolerance. Three suites
had already been caught by it (`sky.cloud_shadows`' wind case accepted a shadow that never moved,
`motion_blur`'s first draft accepted any value, and `water`'s buoyancy case had to be reasoned about
as a fraction), and the macro is used about 1150 times.

## What Changes

- `CY_CHECK_NEAR` passes when `|value - expected| <= tolerance`. Exact equality always passes, a NaN
  never does. Its arguments are converted to `double` explicitly, so an `f32` call site is not an
  implicit promotion under `-Wdouble-promotion`.
- `CY_CHECK_NEAR_REL(value, expected, tolerance)` passes when
  `|value - expected| <= tolerance * max(|value|, |expected|)`, for the call sites whose tolerance
  is a fraction of the quantity; each says why.
- Every call site that the absolute tolerance turns red is audited: code that was wrong is fixed
  with a regression test, a tolerance written as a fraction becomes `CY_CHECK_NEAR_REL`. No
  tolerance is widened.
- `src/core/math/tests/approx.h`'s `CY_CHECK_CLOSE` inherits the change and says so.

## Impact

- `testing-and-quality` — Test infrastructure: the harness's near-comparison is absolute, and a
  relative comparison is written as one.
- Tests only; no engine behaviour changes unless an audited call site exposed a defect.
