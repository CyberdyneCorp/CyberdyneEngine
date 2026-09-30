// SPDX-License-Identifier: MIT
// Every harness macro that expands a `__COUNTER__` or widens a float, once, for
// tests/harness/clang_warnings_test.py to compile with each clang on the host under the project's
// warning set. It is compiled with -fsyntax-only and never linked or run.

#include <cy/test/test.h>

CY_TEST_SUITE("harness.clang_probe") {
    CY_TEST_CASE("the harness compiles cleanly under clang") {
        const float measured = 0.5f;
        CY_CHECK_NEAR(measured, 0.5f, 0.001f);
        CY_CHECK_EQ(measured, 0.5f);
        CY_TEST_SUBCASE("a subcase") {
            CY_TEST_MESSAGE("inside a subcase");
        }
        if (measured > 1.0f) {
            CY_TEST_FAIL_CHECK("unreachable");
            CY_TEST_FAIL("unreachable");
        }
    }
}
