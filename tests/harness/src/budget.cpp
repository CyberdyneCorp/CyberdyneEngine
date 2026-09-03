// The budget half of the harness: every test case is timed, and one that outlives its suite's
// budget fails. See cy/test/test.h.

#include <cy/test/test.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace cy::test {
namespace {

std::uint64_t steady_now_ns() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

// A sanitizer build is five to twenty times slower, and that is the tool working. Holding a
// sanitized run to the unsanitized budget would produce a suite that fails for a reason unrelated
// to the change under test, which is how a check gets switched off.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr double kDefaultScale = 20.0;
#elif defined(__has_feature)
#    if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
        __has_feature(memory_sanitizer)
constexpr double kDefaultScale = 20.0;
#    else
constexpr double kDefaultScale = 1.0;
#    endif
#else
constexpr double kDefaultScale = 1.0;
#endif

double resolve_scale() {
    const char* override_value = std::getenv("CY_TEST_BUDGET_SCALE");
    if (override_value == nullptr || *override_value == '\0') {
        return kDefaultScale;
    }
    char* end = nullptr;
    const double parsed = std::strtod(override_value, &end);
    if (end == override_value || parsed < 0.0) {
        std::fprintf(stderr,
                     "cy::test: CY_TEST_BUDGET_SCALE='%s' is not a non-negative number; using %g\n",
                     override_value, kDefaultScale);
        return kDefaultScale;
    }
    return parsed;
}

}  // namespace

double budget_scale() {
    static const double scale = resolve_scale();
    return scale;
}

namespace {

// A scale that rounds a real budget down to zero would switch the check off, which is what a scale
// of exactly zero means. The two must stay distinguishable, so a scaled budget floors at 1 ns.
unsigned long long scaled_budget(unsigned long long budget_ns) {
    const double scale = budget_scale();
    if (budget_ns == 0 || scale == 0.0) {
        return 0;
    }
    const auto scaled = static_cast<unsigned long long>(static_cast<double>(budget_ns) * scale);
    return scaled == 0 ? 1ULL : scaled;
}

}  // namespace

BudgetGuard::BudgetGuard(const char* name, unsigned long long budget_ns, const char* file, int line)
    : name_(name),
      file_(file),
      line_(line),
      budget_ns_(scaled_budget(budget_ns)),
      started_ns_(steady_now_ns()) {}

BudgetGuard::~BudgetGuard() {
    if (budget_ns_ == 0) {
        return;
    }
    const std::uint64_t elapsed_ns = steady_now_ns() - started_ns_;
    if (elapsed_ns <= budget_ns_) {
        return;
    }

    char message[512];
    std::snprintf(message, sizeof(message),
                  "over budget: '%s' took %.3f ms (%llu ns) against a budget of %.3f ms (%llu ns). "
                  "The taxonomy in `testing-and-quality` places a test this slow in the next suite "
                  "up — move it, or make it fast. Set CY_TEST_BUDGET_SCALE to relax the budget for "
                  "one run.",
                  name_, static_cast<double>(elapsed_ns) / 1e6,
                  static_cast<unsigned long long>(elapsed_ns),
                  static_cast<double>(budget_ns_) / 1e6, budget_ns_);
    DOCTEST_ADD_FAIL_CHECK_AT(file_, line_, message);
}

}  // namespace cy::test
