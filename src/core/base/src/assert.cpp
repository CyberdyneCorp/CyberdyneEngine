#include <cy/core/base/assert.h>

#include <cstdio>
#include <cstdlib>

namespace cy {
namespace {

AssertionHandler g_handler = nullptr;
void* g_handler_user = nullptr;

// Used until src/core/diagnostics/ installs its own (task 3.5.x). One line, on stderr, unbuffered
// by fputs' own flush on abort — a failed assertion that printed nothing would be worse than no
// handler at all.
void default_handler(const AssertionFailure& failure, void* /*user*/) {
    std::fprintf(stderr, "assertion failed: %s\n  at %s:%u in %s\n", failure.expression,
                 failure.file, failure.line, failure.function);
    if (failure.message != nullptr && failure.message[0] != '\0') {
        std::fprintf(stderr, "  %s\n", failure.message);
    }
    std::fflush(stderr);
}

}  // namespace

AssertionHandler set_assertion_handler(AssertionHandler handler, void* user) noexcept {
    AssertionHandler previous = g_handler;
    g_handler = handler;
    g_handler_user = user;
    return previous;
}

void report_assertion_failure(const AssertionFailure& failure) noexcept {
    if (g_handler != nullptr) {
        g_handler(failure, g_handler_user);
    } else {
        default_handler(failure, nullptr);
    }
    std::abort();
}

}  // namespace cy
