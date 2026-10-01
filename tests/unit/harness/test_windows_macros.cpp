// SPDX-License-Identifier: MIT
// <windows.h> defines `near` and `far` as empty macros (minwindef.h), and doctest's implementation
// includes it on Windows, so the harness header has to compile with both names defined away. Its
// near-comparison helpers were first spelled `near` and `near_relative`, which MSVC read as
// `Near (const E& expected, ...)` and refused; this translation unit reproduces that on every host.
#define near
#define far

#include "cy/test/test.h"

CY_TEST_CASE("harness: test.h compiles where windows.h has defined near and far away") {
    CY_CHECK_NEAR(1.0F, 1.0F, 0.0F);
    CY_CHECK_NEAR(2.5, 2.0, 0.5);
    CY_CHECK_NEAR_REL(1001.0, 1000.0, 0.001);
}
