// The budget half of the harness: every test case is timed, and one that outlives its suite's
// budget fails. See cy/test/test.h.
//
// THE CLOCK IS THE TEST'S OWN CPU TIME, NOT THE WALL. That is the whole of task 1.1, and the
// reasoning is in cy/test/test.h beside the class it governs; what lives here is the two clocks and
// the two checks they feed.

#include <cy/test/test.h>

#include "host_blocking.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
// GetThreadTimes is scheduler-quantum coarse (tens of milliseconds), which cannot measure a
// one-millisecond budget. QueryThreadCycleTime returns per-thread TSC cycles, and TSC is invariant
// on every x86_64 CPU this project targets — so a one-shot calibration against
// QueryPerformanceCounter turns cycles into nanoseconds with QPC-level precision. That is what the
// Windows branch of `cpu_now_ns` below does.
#    define NOMINMAX
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#else
#    include <ctime>
#endif

#if defined(__linux__)
#    include <fcntl.h>
#    include <unistd.h>
#endif

namespace cy::test {
namespace {

std::uint64_t steady_now_ns() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

#if defined(_WIN32)

constexpr bool kHaveCpuClock = true;

/// Nanoseconds per TSC cycle, computed once at first use. TSC is invariant on modern x86_64, so a
/// process-wide ratio is correct for every thread and every core. The calibration spins on the
/// calling thread for about ten milliseconds — long enough to make QueryPerformanceCounter noise
/// negligible, short enough that no test observes the pause.
double ns_per_tsc_cycle() {
    static const double ratio = []() -> double {
        LARGE_INTEGER qpc_freq_li{};
        if (!QueryPerformanceFrequency(&qpc_freq_li) || qpc_freq_li.QuadPart <= 0) {
            return 0.0;
        }
        const double qpc_freq = static_cast<double>(qpc_freq_li.QuadPart);
        LARGE_INTEGER qpc_start{};
        std::uint64_t tsc_start = 0;
        if (!QueryPerformanceCounter(&qpc_start) ||
            !QueryThreadCycleTime(GetCurrentThread(), &tsc_start)) {
            return 0.0;
        }
        // Ten milliseconds of busy work. `std::this_thread::sleep_for` would deschedule the thread,
        // and TSC delta is undefined across a deschedule; a spinloop keeps this thread runnable and
        // pinned to a real CPU that increments the counter.
        const double target_seconds = 0.010;
        LARGE_INTEGER qpc_now = qpc_start;
        while ((static_cast<double>(qpc_now.QuadPart - qpc_start.QuadPart) / qpc_freq) <
               target_seconds) {
            QueryPerformanceCounter(&qpc_now);
        }
        LARGE_INTEGER qpc_end = qpc_now;
        std::uint64_t tsc_end = 0;
        if (!QueryThreadCycleTime(GetCurrentThread(), &tsc_end) || tsc_end <= tsc_start) {
            return 0.0;
        }
        const double elapsed_seconds =
            static_cast<double>(qpc_end.QuadPart - qpc_start.QuadPart) / qpc_freq;
        const double elapsed_cycles = static_cast<double>(tsc_end - tsc_start);
        if (elapsed_seconds <= 0.0 || elapsed_cycles <= 0.0) {
            return 0.0;
        }
        return (elapsed_seconds * 1'000'000'000.0) / elapsed_cycles;
    }();
    return ratio;
}

/// The CPU time this thread has consumed, in nanoseconds. Windows path.
///
/// Per-thread rather than per-process: a process clock would count every worker the job system
/// started, so a test that fans one millisecond of work across twenty-four cores would measure
/// twenty-four milliseconds and fail a budget it never came close to spending. The cost of the
/// choice is stated in the header: work a test hands to another thread is not counted here, and the
/// stall ceiling is what still bounds a case that blocks waiting for it.
std::uint64_t cpu_now_ns() {
    std::uint64_t cycles = 0;
    if (!QueryThreadCycleTime(GetCurrentThread(), &cycles)) {
        return steady_now_ns();
    }
    const double ratio = ns_per_tsc_cycle();
    if (ratio <= 0.0) {
        return steady_now_ns();
    }
    return static_cast<std::uint64_t>(static_cast<double>(cycles) * ratio);
}

#elif !defined(CLOCK_THREAD_CPUTIME_ID)

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

/// WHAT THE REFERENCE WORKLOAD DOES NOT SEE: THE OPTIMISER. The loop above is scalar integer
/// arithmetic in registers, and `-O0` slows it by about a fiftieth — the message a case prints in
/// the Debug configuration says "against a budget of 1.022 ms", so the calibration moved by two per
/// cent. It slows the code the suites actually run by a great deal more, because that code is
/// containers, spans, small functions and `Expected<>`, none of which `-O0` inlines. M8.b's closing
/// gate measured the ratio per suite, summing every case's own CPU time in both configurations:
///
///     unit.ecs           6.2x        unit.navigation    4.1x
///     unit.camera_world  2.3x        unit.audio_acoustics 1.5x
///
/// So the budget has been a different budget in Debug since M4 — the header above says an
/// unoptimised build should move it and it did not — and the symptom is a unit tier where a
/// changing handful of suites fails each run while every one of them passes alone. M8.a's gate
/// found sixteen such cases and called its own repair "necessary and not sufficient"; this is the
/// half it did not reach.
///
/// FOUR, AND WHY IT IS NOT MEASURED PER RUN. A second calibration loop written to be
/// optimiser-sensitive would be a benchmark of the optimiser, and its result would move with the
/// compiler rather than with the tests. Four covers every ratio above but the worst, is a constant
/// a reader can check against the table, and leaves the check live: a Debug case doing four times
/// its intended work still fails, and the tier's real number — one millisecond — is enforced in the
/// three configurations that are compiled the way a shipped game is. `four-profiles` runs all four.
#if defined(CY_UNOPTIMISED)
constexpr double kUnoptimisedAllowance = 4.0;
#endif

/// The reference-machine baseline is Leo's Linux/Clang Development build. MSVC in Development
/// (`/O2`) inlines templates less aggressively than Clang at `-O2`, so the container-and-template
/// heavy code the suites actually run is proportionally slower than the scalar-arithmetic reference
/// workload can detect. Measured across the failing cases on this project's Windows/MSVC dev host:
/// the overruns cluster between 1.10x and 1.45x of the unscaled budget. `1.5` closes every one of
/// them with margin while leaving a real regression visible — the ratio at which unit.navigation,
/// unit.foliage and unit.terrain all fell on the wrong side, and above which no case has been
/// observed to fall for compiler-only reasons. Same shape as CY_UNOPTIMISED above, and applied
/// together — a Debug MSVC build gets four times one and a half.
#if defined(_MSC_VER)
constexpr double kMsvcCompilerAllowance = 1.5;
#endif

/// THE SLOWEST OF THREE, not the median, and the reason is the instrument's own worst case.
///
/// This calibration exists to answer "how fast is this machine right now", and its clock counts CPU
/// SECONDS rather than cycles — so a core at 800 MHz makes every case look three times more
/// expensive than the same core at boost. The guard's own message says an IDLE machine is therefore
/// the worst case, and measurement agreed: `unit.weather` failed 8 runs in 25 on a machine doing
/// nothing, and `unit.foliage` carries three cases that cost 1.21 to 1.75 ms against a 1 ms budget
/// even at full clock.
///
/// A MEDIAN HIDES EXACTLY THE SAMPLE THAT MATTERS. The three samples run back to back in a few
/// milliseconds at process start, while the core is still boosted from launch; if one of them
/// catches the governor settling, the median discards it as noise and the budget is set at the
/// boosted clock the cases will not get. Taking the slowest keeps it: a budget that is generous on
/// a down-clocked machine is the correct failure direction, because the alternative is a suite that
/// goes red for being run somewhere quiet.
///
/// It cannot make a budget tighter than the reference, because the ratio still floors at 1.0.
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
    // samples[] is sorted ascending by the three swaps above; [2] is the slowest.
    const auto slowest = static_cast<double>(samples[2]);
    if (slowest <= 0.0) {
        return 1.0;  // no usable clock; the unscaled budget is the honest fallback
    }
    // NEVER BELOW 1.0. A machine faster than the reference does not earn a tighter budget than the
    // one the suite was written against — tightening a budget nobody asked to tighten is how a
    // check starts failing for being run somewhere good.
    const double ratio = slowest / kNominalReferenceNs;
    double machine = ratio < 1.0 ? 1.0 : ratio;
#if defined(CY_UNOPTIMISED)
    machine *= kUnoptimisedAllowance;
#endif
#if defined(_MSC_VER)
    machine *= kMsvcCompilerAllowance;
#endif
    return machine;
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

/// A SECOND OPINION, taken only when a case has apparently blown its budget.
///
/// `budget_scale()` caches `resolve_scale()` in a function-local static, so the calibration is
/// measured ONCE, in the first microseconds of the process. That is the wrong moment on a machine
/// whose governor has not settled: the three reference samples run while the core is still boosted
/// from process launch, the ratio comes out at or below 1.0 and clamps, and then the cases run at
/// 800 MHz against a budget calibrated at full clock. Measured on this project's own host,
/// `unit.weather` failed 8 runs in 25 WHILE THE MACHINE WAS IDLE, reporting the same case at
/// 1.130 ms against a 1.000 ms budget in one run and at 1.672 ms against 1.371 ms in another --
/// two different budgets for one binary, which is the calibration moving rather than the code.
///
/// So an overrun is re-measured before it is believed. The reference workload is taken again, NOW,
/// adjacent to the case that just ran and under the same clock, and the case is only reported over
/// budget if it is still over against that. A real regression is still over; a governor artefact is
/// not. It costs nothing on the passing path, because nothing calls this unless a case looks late.
///
/// An explicit `CY_TEST_BUDGET_SCALE` is never re-measured: somebody who pinned the scale meant it.
[[nodiscard]] double second_opinion_scale() {
    const char* override_value = std::getenv("CY_TEST_BUDGET_SCALE");
    if (override_value != nullptr && *override_value != '\0') {
        return budget_scale();
    }
    return kDefaultScale * measured_scale();
}

double budget_scale() {
    static const double scale = resolve_scale();
    return scale;
}

// --- The third clock: time this thread was RUNNABLE and not running ------------------------------
//
// M9 TASK 7.5b. The stall ceiling below is a WALL-CLOCK assertion, and M8.c's closing gate found
// what that costs: three wall-clock-bound suites each failed once across three full ledger runs
// under the ledger's own sustained load, and each passed three to five times in isolation on an
// idle machine. A gate that is red one run in three teaches people to re-run it.
//
// `testing-and-quality` already says what the answer is: "Budget enforcement SHALL therefore allow
// a stated tolerance for machine variance, and a case that exceeds its budget only under load SHALL
// be reported as a case to reclassify rather than failing the build outright."
//
// The CPU half honours that already — CPU time does not grow when a neighbour spins. The STALL half
// did not, and could not be made to by a tolerance: a case descheduled by forty spinning compilers
// and a case sleeping on a lock look identical in wall clock. THEY ARE NOT IDENTICAL IN
// /proc/thread-self/schedstat, whose second field is the nanoseconds this thread spent on a
// runqueue **wanting a core and not getting one**. Blocking does not accumulate there; preemption
// does, to the nanosecond — a 50 ms window on a saturated machine measured 50.1 ms wall, 24.2 ms
// CPU and 27.0 ms of runqueue wait, and the same window on an idle one measured 50.1, 50.1 and 0.
//
// So the ceiling is applied to wall clock MINUS contention, which is the time the case could have
// been running and was not. A sleep is still caught; a busy machine is reported and not failed.
//
// M11.C'S FOURTH CLOSE FOUND THE HALF THIS DOES NOT SEE: a machine busy with I/O rather than CPU. A
// thread waiting for the disk is not runnable, so it accumulates no runqueue wait, and a case whose
// first touch of its own code took major page faults behind a build's writeback was charged 655 ms
// it never spent. That is the fourth clock, `blocked_on_host_ns()`, in host_blocking.cpp — and
// three closes of trying to excuse it from inside the case ended with the owner's decision that
// NOTHING IS EXCUSED: an uninterruptible wait is reported in the stall message so that the stall is
// explained, and the premise that the host was quiet is checked from outside the process, by
// `tools/quiet-host/` around every ledger criterion that runs a timing-sensitive suite. On that
// premise a case over its ceiling is the case's own, whatever it was waiting for.
std::uint64_t contended_now_ns() {
#if defined(__linux__)
    // One descriptor per thread, `pread` from offset zero: /proc regenerates the contents on each
    // read. Measured at 0.67 us per read, so the two reads a case costs are 0.13 % of the unit
    // tier's budget — small enough that the instrument does not move what it measures.
    static thread_local int descriptor =
        ::open("/proc/thread-self/schedstat", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return 0;
    }
    char buffer[96];
    const ::ssize_t got = ::pread(descriptor, buffer, sizeof(buffer) - 1, 0);
    if (got <= 0) {
        return 0;
    }
    buffer[static_cast<std::size_t>(got)] = '\0';
    // "run_time wait_time timeslices", three integers. `strtoull` rather than `sscanf` because the
    // latter cannot report a conversion failure, and a field this reads as zero would silently
    // excuse every stall rather than none.
    char* after_running = nullptr;
    const unsigned long long running = std::strtoull(buffer, &after_running, 10);
    if (after_running == buffer) {
        return 0;
    }
    (void)running;
    char* after_waiting = nullptr;
    const unsigned long long waiting = std::strtoull(after_running, &after_waiting, 10);
    if (after_waiting == after_running) {
        return 0;
    }
    return static_cast<std::uint64_t>(waiting);
#else
    return 0;
#endif
}

/// How many cases this binary excused as contended. A counter rather than a flag, so a suite that
/// is contended every run is visible as a number rather than as one line lost in the output.
std::atomic<std::uint64_t> contended_cases_{0};

/// THE DECISION, AS A PURE FUNCTION OF THREE NUMBERS, so that it can be tested without a machine
/// in a particular state. The empirical half — that runqueue wait actually moves under load and
/// does not move while blocking — is asserted separately, in `tests/integration/` and
/// `tests/unit/harness/` respectively; this is the arithmetic those two measurements feed.
///
/// THE WALL CLOCK IS OVER THE CEILING in both of the interesting cases, and they are different
/// things: a case that WAITED — a sleep, a blocking read, a lock, a thread it joined — and a case
/// that was simply not given a core while forty other processes wanted one. Only the first is a
/// property of the test. Subtract the time the case spent runnable-and-not-running and what is left
/// is the time it could have been working. Nothing else is subtracted: an uninterruptible wait is
/// the case's own on the quiet host the criteria assume.
StallVerdict stall_verdict(unsigned long long wall_ns, unsigned long long contended_ns,
                           unsigned long long ceiling_ns) noexcept {
    if (ceiling_ns == 0 || wall_ns <= ceiling_ns) {
        return StallVerdict::Fine;
    }
    const unsigned long long attributable = wall_ns > contended_ns ? wall_ns - contended_ns : 0;
    return attributable <= ceiling_ns ? StallVerdict::Contended : StallVerdict::Stalled;
}

unsigned long long contended_ns() noexcept {
    return contended_now_ns();
}

bool budget_measures_contention() noexcept {
#if defined(__linux__)
    // Not "this is Linux": CONFIG_SCHEDSTATS can be off, and /proc can be absent in a container.
    // The harness states which instrument it has rather than assuming one, as it does for the CPU
    // clock, so a test asserting about contention asserts about something that exists.
    static const bool available = []() noexcept {
        const int descriptor = ::open("/proc/thread-self/schedstat", O_RDONLY | O_CLOEXEC);
        if (descriptor < 0) {
            return false;
        }
        ::close(descriptor);
        return true;
    }();
    return available;
#else
    return false;
#endif
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

/// How often the fourth clock samples the case's thread. A two-hundredth of the ceiling keeps the
/// sampling error, a few intervals, under two per cent of the thing it is reported beside; the
/// floor holds the sampler to a thousand wake-ups a second however small the ceiling, and the cap
/// keeps an integration case (a ceiling of 100 s and more) from being sampled so coarsely that a
/// 200 ms disk wait is seen as a handful of points.
unsigned long long host_sampling_interval(unsigned long long ceiling_ns) {
    constexpr unsigned long long kFloor = 1'000'000ULL;
    constexpr unsigned long long kCap = 5'000'000ULL;
    return std::clamp(ceiling_ns / 200ULL, kFloor, kCap);
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
      declared_ns_(budget_ns),
      started_contended_ns_(contended_now_ns()),
      // Before either clock starts, so that starting the sampler — once per process, a thread
      // creation — is charged to no case.
      host_sampled_(kHaveCpuClock && budget_ns_ != 0 &&
                    host_blocking::begin(host_sampling_interval(stall_ceiling(budget_ns_)))),
      started_blocked_ns_(host_sampled_ ? blocked_on_host_ns() : 0),
      started_cpu_ns_(cpu_now_ns()),
      started_wall_ns_(steady_now_ns()) {}

BudgetGuard::~BudgetGuard() {
    if (budget_ns_ == 0) {
        return;
    }
    const std::uint64_t wall_ns = steady_now_ns() - started_wall_ns_;
    const std::uint64_t cpu_ns = cpu_now_ns() - started_cpu_ns_;
    const std::uint64_t contended = contended_now_ns() - started_contended_ns_;
    std::uint64_t blocked = 0;
    if (host_sampled_) {
        blocked = blocked_on_host_ns() - started_blocked_ns_;
        host_blocking::end();
    }

    // THE RE-CHECK. See `second_opinion_scale`: the calibration is taken once at process start and
    // a governor that settles afterwards makes every later case look late. Re-measure now, beside
    // the case that just ran, and let the larger of the two budgets stand.
    unsigned long long budget_ns = budget_ns_;
    if (cpu_ns > budget_ns && declared_ns_ != 0) {
        const double fresh = second_opinion_scale();
        const auto rebuilt =
            static_cast<unsigned long long>(static_cast<double>(declared_ns_) * fresh);
        budget_ns = std::max(budget_ns, rebuilt);
    }

    char message[1536];
    if (cpu_ns > budget_ns) {
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
            static_cast<double>(budget_ns) / 1e6, budget_ns, static_cast<double>(wall_ns) / 1e6);
        DOCTEST_ADD_FAIL_CHECK_AT(file_, line_, message);
        return;
    }

    if constexpr (!kHaveCpuClock) {
        return;
    }
    const unsigned long long ceiling = stall_ceiling(budget_ns);
    const StallVerdict verdict = stall_verdict(wall_ns, contended, ceiling);
    if (verdict == StallVerdict::Fine) {
        return;
    }

    if (verdict == StallVerdict::Contended) {
        ++contended_cases_;
        // REPORTED, NOT FAILED, and `testing-and-quality` asks for exactly that: "a case that
        // exceeds its budget only under load SHALL be reported as a case to reclassify rather than
        // failing the build outright". stderr rather than a check failure, so that `ctest
        // --output-on-failure` shows it beside a real failure and a green run stays green.
        std::fprintf(stderr,
                     "cy::test: contended: '%s' held the suite for %.3f ms of wall clock against a "
                     "ceiling of %.3f ms, of which %.3f ms was spent waiting for a core on a busy "
                     "machine, %.3f ms in an uninterruptible wait (%s, and not excused) and "
                     "%.3f ms was CPU. Reported rather than failed: the machine was loaded, not "
                     "the case. Run it on an idle machine to see its own figure.\n",
                     name_, static_cast<double>(wall_ns) / 1e6, static_cast<double>(ceiling) / 1e6,
                     static_cast<double>(contended) / 1e6, static_cast<double>(blocked) / 1e6,
                     host_sampled_ ? "sampled" : "not measured here",
                     static_cast<double>(cpu_ns) / 1e6);
        return;
    }

    std::snprintf(
        message, sizeof(message),
        "stalled: '%s' held the suite for %.3f ms of wall clock (%llu ns) while spending %.3f ms "
        "of CPU, %.3f ms waiting for a core and %.3f ms in an uninterruptible wait (%s) against a "
        "ceiling of %.3f ms — %llux its budget. A case within its budget that takes this long is "
        "waiting rather than working: a sleep, a lock, a thread it joined, a read from a pipe or "
        "socket, or the disk — its own, or one that a build beside this run kept busy. "
        "`testing-and-quality` places any of those in tests/integration/ or above. Only waiting "
        "for a core is subtracted; an uninterruptible wait is NEVER excused, because nothing a "
        "process can read says whether it or the host caused it (M11.c's fifth to seventh closes). "
        "The suites this ceiling judges are run on a QUIET HOST by their ledger criteria "
        "(`just test-quiet-host -- ...`), so on that premise this is the case's own time. Set "
        "CY_TEST_BUDGET_SCALE to relax both limits for one run.",
        name_, static_cast<double>(wall_ns) / 1e6, static_cast<unsigned long long>(wall_ns),
        static_cast<double>(cpu_ns) / 1e6, static_cast<double>(contended) / 1e6,
        static_cast<double>(blocked) / 1e6, host_sampled_ ? "sampled" : "not measured here",
        static_cast<double>(ceiling) / 1e6, kStallMultiplier);
    DOCTEST_ADD_FAIL_CHECK_AT(file_, line_, message);
}

unsigned long long contended_cases() noexcept {
    return contended_cases_;
}

}  // namespace cy::test
