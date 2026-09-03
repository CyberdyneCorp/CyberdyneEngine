#pragma once
// A three-macro test harness.
//
// The test taxonomy (task 4.1.1) puts doctest behind a CY_TEST_CASE wrapper in tests/, and these
// tests belong in tests/unit/diagnostics/ once that wrapper exists. It did not when this module was
// written, and a second definition of CY_TEST_CASE would be exactly the collision the wrapper
// exists to prevent — so these run as their own executables, registered with add_test(), and move
// when there is something to move them to.

#include <cstdio>
#include <cstring>

namespace cy_test {

inline int g_failures = 0;
inline int g_checks = 0;

inline void check(bool condition, const char* expression, const char* file, int line,
                  const char* message) {
    ++g_checks;
    if (condition) {
        return;
    }
    ++g_failures;
    std::fprintf(stderr, "%s:%d: FAILED %s\n    %s\n", file, line, expression, message);
}

inline int summarise(const char* name) {
    std::fprintf(stderr, "%s: %d checks, %d failed\n", name, g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}

}  // namespace cy_test

#define CY_CHECK(condition, message) \
    ::cy_test::check((condition), #condition, __FILE__, __LINE__, (message))

#define CY_CHECK_EQ(left, right, message) \
    ::cy_test::check((left) == (right), #left " == " #right, __FILE__, __LINE__, (message))
