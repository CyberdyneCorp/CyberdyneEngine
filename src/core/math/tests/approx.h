#pragma once
// A float-friendly spelling of CY_CHECK_NEAR, for the maths suites. Section 3.1.
//
// WHY THIS EXISTS. `CY_CHECK_NEAR` (tests/harness/include/cy/test/test.h) used to expand to
// `doctest::Approx(expected).epsilon(tolerance)`, and `Approx` takes `double`. Every call site that
// passes `f32` — which, in this module, is every call site, because `core-math` fixes runtime
// precision at 32 bits — therefore performs three implicit float-to-double promotions. Clang's
// `-Wdouble-promotion` reports each one, the engine builds with `-Werror`, and the maths tests are
// the first suite in the tree to use `CY_CHECK_NEAR` at all, so nobody had hit it before.
//
// The promotion is intentional: comparing two 32-bit values in 64-bit arithmetic is exactly right.
// So it is written out once, here, rather than sixty times at the call sites or suppressed with a
// pragma that would also hide an unintentional promotion in the code under test.
//
// The harness has since taken that fix: `CY_CHECK_NEAR` converts its arguments to `double`
// explicitly, so an `f32` call site no longer promotes implicitly. This spelling stays because
// sixty call sites use it, and it now means exactly what `CY_CHECK_NEAR` means.

#include <cy/core/base/types.h>

#include <cy/test/test.h>

/// `value` is within `tolerance` of `expected`, compared as doubles. The tolerance is ABSOLUTE,
/// as `CY_CHECK_NEAR`'s is. (It was relative until the harness stopped expanding to
/// `doctest::Approx::epsilon`; the call sites in this module were written as absolute amounts.)
#define CY_CHECK_CLOSE(value, expected, tolerance)                                 \
    CY_CHECK_NEAR(static_cast<::cy::f64>(value), static_cast<::cy::f64>(expected), \
                  static_cast<::cy::f64>(tolerance))
