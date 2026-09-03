// The assertion macros. Task 3.1.2.
//
// CY_ASSERT and CY_ASSERT_MSG state a programmer error — a condition that is true unless the code
// is wrong. They are live in Debug and Development (both define CY_DEVELOPMENT,
// cmake/profiles.cmake) and compiled out of Profile and Shipping. CY_VERIFY evaluates its
// expression in every configuration and only checks the result where the others do, so it is the
// form to use when the expression has an effect.
//
// A failure is routed through an installed handler rather than printed here. That handler is what
// `diagnostics-profiling-and-crash` installs at task 3.5.x: it belongs to src/core/diagnostics/,
// which is a different module written by a different author, and layer 0 has no way to call upward
// into it. This header declares the seam; until something installs a handler, the default one
// writes to stderr and aborts, so an assertion never passes silently.

#pragma once

#include <cy/core/base/types.h>

namespace cy {

struct AssertionFailure {
    const char* expression;  // the text of the condition that was false
    const char* message;     // "" when the assertion carried none
    const char* file;
    const char* function;
    u32 line;
};

// Called on a failed assertion. It may return; the process aborts either way.
using AssertionHandler = void (*)(const AssertionFailure& failure, void* user);

// Installs the handler and returns the previous one, so that a test can restore it. Passing nullptr
// restores the default. `user` is passed back to the handler untouched.
AssertionHandler set_assertion_handler(AssertionHandler handler, void* user) noexcept;

// Reports through the installed handler, then aborts. Never returns: the condition is a programmer
// error, and there is no Expected for it to become.
[[noreturn]] void report_assertion_failure(const AssertionFailure& failure) noexcept;

}  // namespace cy

#if defined(CY_DEVELOPMENT)

#    define CY_ASSERT_MSG(expression, message)                        \
        (static_cast<bool>(expression)                                \
             ? void(0)                                                \
             : ::cy::report_assertion_failure(::cy::AssertionFailure{ \
                   #expression, (message), __FILE__, __func__, static_cast<::cy::u32>(__LINE__)}))

#    define CY_VERIFY(expression) CY_ASSERT_MSG(expression, "")

#else

// The expression is not evaluated, but it is still parsed and its names are still used, so a
// variable that exists only to be asserted about does not become an unused-variable warning in
// Shipping. sizeof() is unevaluated, which is exactly that property.
#    define CY_ASSERT_MSG(expression, message) \
        ((void)sizeof(static_cast<bool>(expression)), (void)sizeof(message), (void)0)

// CY_VERIFY always evaluates: its expression is allowed to have an effect.
#    define CY_VERIFY(expression) ((void)(expression))

#endif  // CY_DEVELOPMENT

#define CY_ASSERT(expression) CY_ASSERT_MSG(expression, "")
