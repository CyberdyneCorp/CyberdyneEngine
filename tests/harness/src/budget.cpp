// The budget half of the harness: every test case is timed, and one that outlives its suite's
// budget fails. See cy/test/test.h.
//
// THE CLOCK IS THE TEST'S OWN CPU TIME, NOT THE WALL. That is the whole of task 1.1, and the
// reasoning is in cy/test/test.h beside the class it governs; what lives here is the two clocks and
// the two checks they feed.

#include <cy/test/test.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
// Windows keeps the wall clock: GetThreadTimes reports in 100 ns units but is updated on the
// scheduler's quantum — tens of milliseconds — which cannot measure a one-millisecond budget at
// all. Nothing in this repository has ever been compiled on Windows, so this is the honest state:
// the platform that can be measured gets the fix, and the platform that cannot keeps the behaviour
// it has always had, named rather than silently different.
#else
#    include <ctime>
#endif

namespace cy::test {
namespace {

std::uint64_t steady_now_ns() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

#if defined(_WIN32) || !defined(CLOCK_THREAD_CPUTIME_ID)

constexpr bool kHaveCpuClock = false;

std::uint64_t cpu_now_ns() {
    return steady_now_ns();
}

#else

constexpr bool kHaveCpuClock = true;

/// The CPU time this thread has consumed, in nanoseconds.
///
/// Per-thread rather than per-process. A process clock would count every worker the job system
/// started, so a test that fans one millisecond of work across twenty-four cores would measure
/// twenty-four milliseconds and fail a budget it never came close to spending. The cost of the
/// choice is stated in the header: work a test hands to another thread is not counted here, and the
/// stall ceiling is what still bounds a case that blocks waiting for it.
std::uint64_t cpu_now_ns() {
    timespec now{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0) {
        return steady_now_ns();
    }
    return (static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL) +
           static_cast<std::uint64_t>(now.tv_nsec);
}

#endif

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

/// The reference workload, and the number the budget is really expressed in.
///
/// WHY A BUDGET NEEDS CALIBRATING AT ALL. `CY_TEST_BUDGET_ns` is written as a duration, and a
/// duration is not a property of the test — it is a property of the test AND the machine AND the
/// optimiser AND whatever else is running. Two gates measured that from opposite ends: M7's found
/// the same suite taking 1.05 ms alone and 0.20 ms beside four spinners, because this governor
/// idles at 800 MHz and an IDLE machine is therefore the harness's worst case; M8.a's found five of
/// seven Debug failures passing standalone and failing only inside a 57-suite run. So the budget
/// has been failing on the machine's state rather than on the code, since M4, and every milestone
/// that added a unit suite made it worse for every existing one without changing a line of them.
///
/// So the budget is calibrated. This loop does a fixed amount of arithmetic; its cost is measured
/// once per binary, and the budget is scaled by how that measurement compares to a nominal. An
/// unoptimised build, a throttled core and a contended one all make it slower together, which is
/// exactly the set of things that were making tests fail.
///
/// WHAT IT IS NOT. It is not a benchmark and its absolute value means nothing. It is a ratio, and
/// the only property that matters is that it moves the same way the tests do.
[[nodiscard]] std::uint64_t reference_workload_ns() {
    // `volatile` so neither the optimiser nor the linker can decide this loop is unobservable and
    // delete it — a calibration that compiles to nothing reports every machine as infinitely fast.
    static volatile std::uint64_t sink = 0;
    const std::uint64_t started = cpu_now_ns();
    std::uint64_t value = 0x9E3779B97F4A7C15ULL;
    for (int index = 0; index < 200000; ++index) {
        value ^= value >> 30U;
        value *= 0xBF58476D1CE4E5B9ULL;
        value ^= value >> 27U;
        value += static_cast<std::uint64_t>(index);
    }
    sink = value;
    const std::uint64_t elapsed = cpu_now_ns() - started;
    // Read it back as well as writing it: a `volatile` store is enough for the optimiser but not
    // for `-Wunused-but-set-variable`, and silencing that warning rather than satisfying it would
    // leave the next reader unsure whether the loop is observable.
    return elapsed + (sink & 0ULL);
}

/// What `reference_workload_ns` costs on a machine the budgets were written for: this project's
/// development machine, Development configuration, otherwise idle. Measured rather than chosen, and
/// it is the only number here that is allowed to be arbitrary — everything else is a ratio to it.
constexpr double kNominalReferenceNs = 900000.0;

/// The median of three, because the calibration is itself a measurement and the thing it is
/// measuring is variance.
[[nodiscard]] double measured_scale() {
    std::uint64_t samples[3] = {};
    for (auto& sample : samples) {
        sample = reference_workload_ns();
    }
    if (samples[0] > samples[1]) {
        std::swap(samples[0], samples[1]);
    }
    if (samples[1] > samples[2]) {
        std::swap(samples[1], samples[2]);
    }
    if (samples[0] > samples[1]) {
        std::swap(samples[0], samples[1]);
    }
    const auto median = static_cast<double>(samples[1]);
    if (median <= 0.0) {
        return 1.0;  // no usable clock; the unscaled budget is the honest fallback
    }
    // NEVER BELOW 1.0. A machine faster than the reference does not earn a tighter budget than the
    // one the suite was written against — tightening a budget nobody asked to tighten is how a
    // check starts failing for being run somewhere good.
    const double ratio = median / kNominalReferenceNs;
    return ratio < 1.0 ? 1.0 : ratio;
}

double resolve_scale() {
    const char* override_value = std::getenv("CY_TEST_BUDGET_SCALE");
    if (override_value == nullptr || *override_value == '\0') {
        return kDefaultScale * measured_scale();
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

bool budget_measures_cpu_time() noexcept {
    return kHaveCpuClock;
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

/// The wall-clock ceiling for a case whose budget is `budget_ns`. See kStallMultiplier.
unsigned long long stall_ceiling(unsigned long long budget_ns) {
    if (budget_ns == 0) {
        return 0;
    }
    // Saturate rather than wrap: a caller may hand a budget large enough that the multiplication
    // overflows, and a ceiling of nearly zero would fail every case.
    constexpr unsigned long long kMaximum = ~0ULL;
    if (budget_ns > kMaximum / kStallMultiplier) {
        return kMaximum;
    }
    return budget_ns * kStallMultiplier;
}

}  // namespace

BudgetGuard::BudgetGuard(const char* name, unsigned long long budget_ns, const char* file, int line)
    : name_(name),
      file_(file),
      line_(line),
      budget_ns_(scaled_budget(budget_ns)),
      started_cpu_ns_(cpu_now_ns()),
      started_wall_ns_(steady_now_ns()) {}

BudgetGuard::~BudgetGuard() {
    if (budget_ns_ == 0) {
        return;
    }
    const std::uint64_t cpu_ns = cpu_now_ns() - started_cpu_ns_;
    const std::uint64_t wall_ns = steady_now_ns() - started_wall_ns_;

    char message[640];
    if (cpu_ns > budget_ns_) {
        std::snprintf(
            message, sizeof(message),
            "over budget: '%s' spent %.3f ms of CPU (%llu ns) against a budget of %.3f ms "
            "(%llu ns), in %.3f ms of wall clock. The taxonomy in `testing-and-quality` places a "
            "test this expensive in the next suite up — move it, or make it cheaper. The clock is "
            "the case's own CPU time, which counts SECONDS rather than cycles: on a host whose "
            "governor idles at 800 MHz, M7's gate measured the same case at five times its "
            "boosted-clock figure, so an IDLE machine is this instrument's worst case. Check the "
            "margin with CY_TEST_BUDGET_SCALE=0.5 before believing a regression.",
            name_, static_cast<double>(cpu_ns) / 1e6, static_cast<unsigned long long>(cpu_ns),
            static_cast<double>(budget_ns_) / 1e6, budget_ns_, static_cast<double>(wall_ns) / 1e6);
        DOCTEST_ADD_FAIL_CHECK_AT(file_, line_, message);
        return;
    }

    const unsigned long long ceiling = stall_ceiling(budget_ns_);
    if (!kHaveCpuClock || wall_ns <= ceiling) {
        return;
    }
    std::snprintf(
        message, sizeof(message),
        "stalled: '%s' held the suite for %.3f ms of wall clock (%llu ns) while spending %.3f ms "
        "of CPU, against a ceiling of %.3f ms — %llux its budget. A case within its budget that "
        "takes this long is waiting rather than working: a sleep, a blocking read, a lock, or a "
        "thread it joined. `testing-and-quality` places any of those in tests/integration/ or "
        "above. Set CY_TEST_BUDGET_SCALE to relax both limits for one run.",
        name_, static_cast<double>(wall_ns) / 1e6, static_cast<unsigned long long>(wall_ns),
        static_cast<double>(cpu_ns) / 1e6, static_cast<double>(ceiling) / 1e6, kStallMultiplier);
    DOCTEST_ADD_FAIL_CHECK_AT(file_, line_, message);
}

}  // namespace cy::test
