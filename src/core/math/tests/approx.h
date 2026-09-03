#pragma once
// A float-friendly spelling of CY_CHECK_NEAR, for the maths suites. Section 3.1.
//
// WHY THIS EXISTS. `CY_CHECK_NEAR` (tests/harness/include/cy/test/test.h) expands to
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
// The better fix is in the harness — `CY_CHECK_NEAR` could take its arguments as `double` by
// construction — but tests/harness/ is not this module's to edit. Flagged for whoever closes M1.

#include <cy/core/base/types.h>

#include <cy/test/test.h>

/// `value` is within `tolerance` of `expected`, compared as doubles. The tolerance is **relative**,
/// because that is what `doctest::Approx::epsilon` means: it scales by the larger magnitude. An
/// expected value of exactly zero therefore admits no tolerance at all, and a comparison against
/// zero must be written as an explicit `std::fabs(x) <= t` instead.
#define CY_CHECK_CLOSE(value, expected, tolerance)                                 \
    CY_CHECK_NEAR(static_cast<::cy::f64>(value), static_cast<::cy::f64>(expected), \
                  static_cast<::cy::f64>(tolerance))
