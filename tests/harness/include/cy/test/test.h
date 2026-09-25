// cy/test/test.h — the test vocabulary. The only file in the repository that includes doctest.
//
// Task 4.1.1, design.md §5. doctest is the framework because it compiles roughly an order of
// magnitude faster than the alternative, and `testing-and-quality` budgets thousands of unit tests
// at under a millisecond each. That argument is about today's numbers, so the framework is reached
// through one wrapper: replacing it is a change to this header and to tests/harness/, not to every
// test in the tree. tests/CMakeLists.txt fails the configure if any other test source includes
// doctest directly, so the seam cannot decay quietly.
//
// The wrapper is thin on purpose. It renames what doctest already does well and adds exactly one
// thing doctest does not do: it enforces the taxonomy's per-test budget, because a budget nothing
// measures is an aspiration. That budget is CPU time rather than wall clock, which is task 1.1 and
// is argued where BudgetGuard is declared.

#ifndef CY_TEST_TEST_H
#define CY_TEST_TEST_H

// MSVC's `<string_view>` (as of v19.44) declares an `operator<<` overload for `basic_ostream` that
// references `basic_ostream::iostate` inline. `basic_ostream` is only forward-declared in that
// header, so a translation unit that pulls in `<string_view>` without first including `<ostream>`
// fails to compile inside the STL. Every test that uses `<string_view>` would otherwise need the
// workaround; declaring it here in the one file every test includes keeps the fix in one place.
// GCC and Clang STLs do not have this dependency but including `<ostream>` here is a no-op cost.
#if defined(_MSC_VER)
#    include <ostream>
#endif

#include <doctest/doctest.h>

#include <cstddef>

// The budget for one test case, in nanoseconds of the case's own CPU time. tests/CMakeLists.txt
// defines it per suite from the taxonomy in `testing-and-quality`: 1 ms for a unit test, 1 s for an
// integration test, 30 s for a smoke test. The default here is the strictest of the three, so a
// test built outside that machinery is held to the tightest budget rather than to none.
#ifndef CY_TEST_BUDGET_NS
#    define CY_TEST_BUDGET_NS 1000000ULL
#endif

namespace cy::test {

/// How much wall clock a case within its budget may still hold the suite for, as a multiple of the
/// budget. See BudgetGuard: the budget is CPU time, and this is the separate limit that keeps
/// "a unit test does not sleep, block or wait on a thread" a checked property rather than a
/// comment.
///
/// A hundred, because the two limits must not be confusable. A case that spends 1 ms of CPU and
/// 100 ms of wall clock is waiting on something by a factor no amount of machine load explains: the
/// measurement that motivated this file saw a 0.2 ms case stretch to 4.1 ms of wall clock under
/// 24-way oversubscription, which is 25x below this ceiling.
inline constexpr unsigned long long kStallMultiplier = 100;

/// Fails the surrounding test case when its body costs more than its suite's budget.
///
/// Constructed by CY_TEST_CASE, so every test in the tree is measured. A failure is reported as an
/// ordinary check failure at the test's own file and line — it names the budget, the measurement
/// and the recipe that owns the suite, because "which suite is this test in?" is the first question
/// an over-budget test raises.
///
/// --- THE CLOCK IS THE CASE'S OWN CPU TIME, AND THAT IS TASK 1.1 -------------------------------
///
/// Through M2 the budget was wall clock, and `four-profiles` — an exit criterion of two milestones
/// and a permanent gate, so a pull request is exposed to it three times over — failed about one
/// Debug run in thirty for it. The failure was measured, not guessed: `ctest -L unit` in the Debug
/// tree fails when the host is busy and passes when it is idle (0 failures in 100 idle runs; 10 in
/// 30 with 24 spinning threads beside it), and the case it lands on is whichever one happened to be
/// descheduled — `unit.values`' generation-table case measured 4.134 ms of wall clock against a
/// 1 ms budget while doing about 0.2 ms of work.
///
/// That is a defect in the instrument. The taxonomy's question is "what does this test cost?", and
/// the answer must not change with what else the machine is doing, or the gate reports on the
/// build agent rather than on the change. So the budget is the CPU time the case's own thread
/// consumed, and the split that moved seven scene and reflection cases into the integration suite
/// at M2's close stands on exactly those numbers.
///
/// THE CLOCK IS STILL NOT INDEPENDENT OF THE MACHINE, AND M7'S GATE MEASURED WHICH WAY. A thread
/// CPU clock counts SECONDS, not cycles, so what it reports is work divided by the frequency the
/// governor happened to be running at. On a host with `scaling_governor = powersave`, cores idling
/// at 800 MHz and boosting to several GHz, that ratio moves by a factor of five — and it moves in
/// the OPPOSITE direction to the one this comment used to assume. The same binary,
/// `cy_test_unit_scene` in the Debug tree, worst case per run:
///
///     idle machine, five runs        0.716  0.734  0.877  0.939  0.846 ms
///     four spinners beside it        0.211  0.206  0.200   —      —    ms
///     idle again, five runs          1.049  0.922  0.702  0.904  0.727 ms
///
/// **An idle machine is this instrument's worst case**, because an idle machine is a slow one; a
/// busy machine that keeps the cores boosted measures the same work at a fifth of the figure. The
/// 1.049 ms row is a case doing about 0.21 ms of work failing a 1 ms budget with nothing else
/// running. That is the mechanism behind "one unit suite failed once" in every milestone report
/// since M4, and it is not contention, not reclaim and not the suites.
///
/// Swapping wall clock for CPU time was still right — wall clock moved by ten — and the remaining
/// factor is not zero. Until the budget is normalised against a reference workload measured in the
/// same process, treat any case within a factor of two of its budget as a case that will fail
/// somewhere: `CY_TEST_BUDGET_SCALE=0.5 ./cy_test_unit_<suite>` lists them. M7's gate moved the
/// three that were worst (`material_lowering`, the cookie scroll case and the overlay clipping
/// case) into the integration suite, which raises the margin but does not fix the instrument.
///
/// The fix deliberately is NOT any of: shrinking a case (the case is not the problem), raising the
/// budget (a budget raised until a flake hides measures nothing), or scaling it under load (which
/// is the same thing with an extra variable).
///
/// WHAT THE CPU CLOCK DOES NOT SEE, AND WHAT COVERS IT. Time the case spends waiting — a sleep, a
/// blocking read, a lock, a thread it joined — costs its thread no CPU, and work handed to the job
/// system is charged to the workers rather than to the case. A pure CPU budget would therefore let
/// a unit test sleep for a second, and `testing-and-quality` places sleeping and I/O in
/// tests/integration/ or above. `kStallMultiplier` is the second limit that keeps that checkable:
/// a case within its CPU budget that holds the suite for more than a hundred times it is reported
/// as stalled, and the message says "waiting" rather than "slow", because that is what it is.
///
/// THE STALL CEILING IS WALL CLOCK, AND WALL CLOCK ASSUMES A QUIET HOST. Waiting for a core is
/// subtracted (`contended_ns`, M9 task 7.5b) because the kernel counts it exactly. Nothing else
/// is: M11.c's fifth, sixth and seventh closes each refuted an allowance that tried to excuse an
/// uninterruptible wait as the host's I/O, every time with a case that made the wait itself, and
/// the owner's decision was that the premise be stated and checked from outside instead:
/// `just test-quiet-host -- ...` (tools/quiet-host/) fails with "host too busy:" rather than
/// letting a loaded machine's figure stand for the case's. And since M11.c's ninth close (option
/// B) the ceiling FAILS a case only when the run is inside that wrapper, verified through /proc;
/// anywhere else the stall is reported, "not enforced: not on a quiet host", and the case passes
/// unless its CPU budget failed. See cy/test/quiet_host.h.
///
/// The budget is scaled by the CY_TEST_BUDGET_SCALE environment variable, and defaults to a relaxed
/// scale under a sanitizer, where a five- to twenty-fold slowdown is the tool working correctly
/// rather than the test regressing. CY_TEST_BUDGET_SCALE=0 disables both checks.
///
/// AND BY A FIXED ALLOWANCE IN THE Debug CONFIGURATION, which is the half M8.a's repair did not
/// reach. The calibration loop is scalar integer arithmetic that `-O0` slows by two per cent, while
/// it slows the container- and abstraction-heavy code the suites run by one and a half to six times
/// — so the budget was silently a different budget in Debug, and the symptom was a changing handful
/// of suites failing each run while every one of them passed alone. `budget.cpp` carries the
/// per-suite measurement and the constant. The tier's real number is enforced unchanged in the
/// three configurations compiled the way a shipped game is, and `four-profiles` runs all four.
class BudgetGuard {
public:
    BudgetGuard(const char* name, unsigned long long budget_ns, const char* file, int line);
    ~BudgetGuard();

    BudgetGuard(const BudgetGuard&) = delete;
    BudgetGuard& operator=(const BudgetGuard&) = delete;

private:
    const char* name_;
    const char* file_;
    int line_;
    unsigned long long budget_ns_;
    /// The budget as DECLARED, before the process-start calibration scaled it. Kept so that an
    /// apparent overrun can be re-checked against a scale measured beside the case rather than in
    /// the first microseconds of the process — see `second_opinion_scale` in budget.cpp.
    unsigned long long declared_ns_;
    unsigned long long started_contended_ns_;
    /// Whether this guard's thread is being sampled for uninterruptible waits, either as the
    /// sampling session's owner or nested in another guard's session. See `blocked_on_host_ns`.
    bool host_sampled_;
    /// The fourth clock's reading when the guard started.
    unsigned long long started_blocked_ns_;
    unsigned long long started_cpu_ns_;
    unsigned long long started_wall_ns_;
};

/// The scale applied to every budget, resolved once from the environment. Exposed so that a test of
/// the harness can state what it is running under rather than guess.
double budget_scale();

/// True where the budget is measured as CPU time. False on a platform with no usable per-thread CPU
/// clock — Windows, whose thread times are updated on the scheduler's quantum — where the guard
/// falls back to wall clock and the stall ceiling is not applied. Exposed so that a test of the
/// harness states which instrument it is asserting about instead of assuming one.
bool budget_measures_cpu_time() noexcept;

/// Nanoseconds this thread has spent RUNNABLE AND NOT RUNNING, cumulative, or zero where the
/// platform does not report it.
///
/// M9 TASK 7.5b. The stall ceiling is a wall-clock assertion and wall clock is the machine's
/// property as much as the test's — which is the defect the CPU budget already fixed once, left in
/// the one check that cannot use a CPU clock. A case cannot tell, from wall clock alone, whether it
/// waited or was preempted. This clock can: Linux's per-thread `schedstat` counts runqueue wait,
/// which preemption grows and blocking does not. The guard subtracts it before applying the
/// ceiling, so a busy machine is REPORTED and a sleeping case still FAILS.
unsigned long long contended_ns() noexcept;

/// True where `contended_ns()` measures something. False elsewhere, and then the stall ceiling
/// behaves exactly as it did before — named rather than silently different.
bool budget_measures_contention() noexcept;

/// Nanoseconds the calling thread has been observed in an UNINTERRUPTIBLE sleep — which is where
/// Linux puts a thread waiting for a block device, a major page fault, writeback or its own
/// `vfork` child — cumulative over the case's sampling session. Zero when this thread is not the
/// one being sampled, and on every platform where `budget_measures_host_blocking()` is false.
///
/// M11.c's fourth close. Runqueue wait is what a CPU-saturated machine costs a case; this is what
/// an I/O-saturated one costs it, and it is invisible to `contended_ns()` because a thread waiting
/// for the disk is not runnable. `unit.determinism` measured it: 655 ms of wall clock, 0.21 ms of
/// CPU, zero runqueue wait, beside a heavy build. A futex, a sleep, a join and a pipe read are
/// INTERRUPTIBLE sleeps and do not grow it.
///
/// IT IS A DIAGNOSTIC AND NEVER AN EXCUSE. An uninterruptible wait says the kernel made the case
/// wait and does not say what for: a case's own `vfork` child, its own `fsync`, its own uncached
/// reads and its own helpers pressing the disk are all uninterruptible too. M11.c's fifth, sixth
/// and seventh closes each refuted an attempt to tell those from the host's I/O from inside the
/// case, and the owner's decision was to stop: the guard subtracts NOTHING for this clock, the
/// stall message reports it so that a stall behind a build's writeback is explained, and the
/// premise that the host was quiet is checked from OUTSIDE the process — `tools/quiet-host/`,
/// around every ledger criterion that runs a timing-sensitive suite. How the clock is measured,
/// and its limits, are in `host_blocking.cpp`.
unsigned long long blocked_on_host_ns() noexcept;

/// True where `blocked_on_host_ns()` measures something.
bool budget_measures_host_blocking() noexcept;

/// The scheduler state letter of a `/proc/<pid>/task/<tid>/stat` line — `R`, `S`, `D` and the
/// rest — or `'\0'` when the text is not one. Exposed because the state is found after the LAST
/// `)`, a thread may name itself anything including `) D (`, and that is worth a test of its own.
char thread_state_from_stat(const char* text, std::size_t size) noexcept;

/// What the wall-clock half of the budget concluded about one case.
enum class StallVerdict {
    /// Inside the ceiling, or no ceiling at all.
    Fine,
    /// Over the ceiling, and the time over it was spent runnable and waiting for a core on a busy
    /// machine. Reported and not failed: `testing-and-quality` asks for "a case that exceeds its
    /// budget only under load" to be reported as a case to reclassify rather than failing the
    /// build.
    Contended,
    /// Over the ceiling on the case's own account: a sleep, a lock, a join, a read from a pipe or
    /// a socket, or an uninterruptible wait — its own, or the disk's, which on the quiet host the
    /// criteria assume is the case's too.
    Stalled,
};

/// The stall decision, as a pure function of three numbers. Exposed so that the arithmetic is
/// testable without arranging for a machine to be busy: the empirical claims underneath it — that
/// runqueue wait grows under preemption and does not grow while blocking — are asserted
/// separately, and this is what they feed. `contended_ns` is runqueue wait and nothing else: the
/// host-blocking clock is never part of it.
StallVerdict stall_verdict(unsigned long long wall_ns, unsigned long long contended_ns,
                           unsigned long long ceiling_ns) noexcept;

/// How many cases this binary excused because the machine, rather than the case, was slow.
unsigned long long contended_cases() noexcept;

}  // namespace cy::test

// --- Test declaration ---------------------------------------------------------------------------

#define CY_TEST_CONCAT_IMPL(a, b) a##b
#define CY_TEST_CONCAT(a, b) CY_TEST_CONCAT_IMPL(a, b)

#ifdef __COUNTER__
#    define CY_TEST_UNIQUE(prefix) CY_TEST_CONCAT(prefix, __COUNTER__)
#else
#    define CY_TEST_UNIQUE(prefix) CY_TEST_CONCAT(prefix, __LINE__)
#endif

// The body is a separate function so that the budget guard brackets it exactly: the guard is
// constructed before the first statement and destroyed after the last, including on an early
// return. Subcases still work — doctest tracks them on a stack, so a CY_TEST_SUBCASE inside the
// body function, or inside a helper it calls, behaves as it does inside a bare test case.
#define CY_TEST_CASE_IMPL(name, fn)                                                      \
    static void fn();                                                                    \
    DOCTEST_TEST_CASE(name) {                                                            \
        ::cy::test::BudgetGuard cy_test_budget_guard_{name, CY_TEST_BUDGET_NS, __FILE__, \
                                                      __LINE__};                         \
        fn();                                                                            \
    }                                                                                    \
    static void fn()

/// Declare a test case. `CY_TEST_CASE("name") { ... }`
#define CY_TEST_CASE(name) CY_TEST_CASE_IMPL(name, CY_TEST_UNIQUE(cy_test_body_))

/// A named section of a test case, re-entered once per leaf. State declared before it is rebuilt
/// for each, which is how a fixture is shared without being shared between runs.
#define CY_TEST_SUBCASE(name) DOCTEST_SUBCASE(name)

/// Group the test cases that follow under a name, for `--test-suite=` selection.
#define CY_TEST_SUITE(name) DOCTEST_TEST_SUITE(name)

// --- Assertions ---------------------------------------------------------------------------------
//
// CY_CHECK records a failure and carries on; CY_REQUIRE stops the test, because what follows it
// would be meaningless. Under -fno-exceptions a failed CY_REQUIRE aborts the process rather than
// unwinding — that is doctest's DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS behaviour, set in
// cmake/dependencies.cmake, and it is the right trade: the alternative is a precondition failure
// that silently continues into the code it was guarding. It does mean a failed CY_REQUIRE takes the
// rest of the binary's test cases with it, so guard preconditions with it and assert results with
// CY_CHECK.

#define CY_CHECK(...) DOCTEST_CHECK(__VA_ARGS__)
#define CY_CHECK_FALSE(...) DOCTEST_CHECK_FALSE(__VA_ARGS__)
#define CY_CHECK_EQ(...) DOCTEST_CHECK_EQ(__VA_ARGS__)
#define CY_CHECK_NE(...) DOCTEST_CHECK_NE(__VA_ARGS__)
#define CY_CHECK_LT(...) DOCTEST_CHECK_LT(__VA_ARGS__)
#define CY_CHECK_LE(...) DOCTEST_CHECK_LE(__VA_ARGS__)
#define CY_CHECK_GT(...) DOCTEST_CHECK_GT(__VA_ARGS__)
#define CY_CHECK_GE(...) DOCTEST_CHECK_GE(__VA_ARGS__)

#define CY_REQUIRE(...) DOCTEST_REQUIRE(__VA_ARGS__)
#define CY_REQUIRE_FALSE(...) DOCTEST_REQUIRE_FALSE(__VA_ARGS__)
#define CY_REQUIRE_EQ(...) DOCTEST_REQUIRE_EQ(__VA_ARGS__)
#define CY_REQUIRE_NE(...) DOCTEST_REQUIRE_NE(__VA_ARGS__)

/// Floating-point comparison with an explicit tolerance. There is no default tolerance: the value
/// that is close enough is a property of what is being measured, not of the framework.
#define CY_CHECK_NEAR(value, expected, tolerance) \
    DOCTEST_CHECK((value) == doctest::Approx(expected).epsilon(tolerance))

/// Record a message in the test's output without asserting anything.
#define CY_TEST_MESSAGE(...) DOCTEST_MESSAGE(__VA_ARGS__)

/// Fail the current test case with a message, and continue.
#define CY_TEST_FAIL_CHECK(...) DOCTEST_FAIL_CHECK(__VA_ARGS__)

/// Fail the current test case with a message, and stop it.
#define CY_TEST_FAIL(...) DOCTEST_FAIL(__VA_ARGS__)

#endif  // CY_TEST_TEST_H
