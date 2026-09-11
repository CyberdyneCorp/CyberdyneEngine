// The other half of M9 task 7.5b's proof: a case preempted by a busy machine IS excused.
//
// `tests/unit/harness/test_budget.cpp` holds the two halves that need no machine state — the stall
// verdict as a pure function of three numbers, and the negative control that a case which BLOCKS
// accumulates no runqueue wait, so a sleep is still a stall. This is the positive one, and it is in
// `integration` rather than `unit` for the reason the taxonomy gives: it oversubscribes a core for
// a tenth of a second, which is a hundred times the unit tier's budget.
//
// WHY IT MATTERS. `smoke.editor_window`, `unit.render_gpu_culling` and the editor's
// `across_a_process_boundary` each failed ONCE across three full ledger runs under the ledger's own
// sustained load, and each passed three to five times in isolation on an idle machine. M8.c's
// closing gate recorded that as task 7.5b and could not fix it inside its own scope: "a gate that
// is red one run in three teaches people to re-run it". The fix is the third clock, and this is the
// case that says the third clock sees what it is supposed to see.
//
// ================================================================================================
// WHY IT PINS ITSELF TO ONE CORE, WHICH IS THE MEASUREMENT AND NOT A TRICK
// ================================================================================================
//
// The first version created ninety-six spinning threads on a twenty-four core machine and measured
// **zero** contention, repeatably. That is not a bug in the clock: with an idle machine and one
// process, the load balancer gives the measuring thread a core of its own and packs the spinners
// onto the others, so nothing ever waits on a runqueue. The wait this instrument exists to see is
// the one a *busy machine* produces — forty compiler processes in another session — and a test may
// not start forty processes to prove it.
//
// Restricting this thread and its spinners to a single CPU produces exactly that condition inside
// one process, deterministically: nine runnable threads, one core, and the measuring thread spends
// most of its window enqueued. Measured across runs: 69-76 ms of wall clock for a 60 ms window, of
// which 59-66 ms was runqueue wait. The affinity mask is restored afterwards, and a platform that
// refuses to set one reports that rather than asserting on a condition it could not create.

#include <cy/test/test.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

#if defined(__linux__)
#    include <sched.h>
#endif

namespace {

/// Spin until told to stop, announcing readiness first.
///
/// THE DEADLINE IS NOT SET IN ADVANCE, and the first version of this file got that wrong in a way
/// worth keeping written down: it computed `now + 120 ms` before creating the spinners, and
/// creating them can take longer than that. Every spinner then returned immediately, the
/// measurement window had the machine to itself, and the case failed with 0.004 ms of contention —
/// a test measuring how long a thread constructor takes rather than what it claims to measure. It
/// passed when run alone and failed inside the suite, which is the signature.
void spin_until_stopped(const std::atomic<bool>& stop, std::atomic<unsigned>& ready) {
    ready.fetch_add(1, std::memory_order_relaxed);
    volatile unsigned sink = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        for (unsigned index = 0; index < 1024U; ++index) {
            sink = sink + index;
        }
    }
    (void)sink;
}

}  // namespace

CY_TEST_CASE("harness: a case preempted by a busy machine accumulates contention, and is excused") {
    if (!cy::test::budget_measures_contention()) {
        CY_TEST_MESSAGE(
            "this platform does not report runqueue wait; the stall ceiling is unchanged");
        return;
    }

#if !defined(__linux__)
    CY_TEST_MESSAGE(
        "this platform cannot pin a thread to one core; the condition cannot be created");
#else
    cpu_set_t original;
    CPU_ZERO(&original);
    if (::sched_getaffinity(0, sizeof(original), &original) != 0) {
        CY_TEST_MESSAGE("the affinity mask could not be read; nothing is asserted");
        return;
    }
    cpu_set_t single;
    CPU_ZERO(&single);
    CPU_SET(0, &single);
    if (::sched_setaffinity(0, sizeof(single), &single) != 0) {
        CY_TEST_MESSAGE(
            "this environment refuses an affinity mask; the condition cannot be created");
        return;
    }

    constexpr unsigned kSpinners = 8;
    std::atomic<bool> stop{false};
    std::atomic<unsigned> ready{0};
    std::vector<std::thread> spinners;
    spinners.reserve(static_cast<std::size_t>(kSpinners));
    for (unsigned index = 0; index < kSpinners; ++index) {
        spinners.emplace_back([&]() { spin_until_stopped(stop, ready); });
    }
    while (ready.load(std::memory_order_relaxed) < kSpinners) {
        std::this_thread::yield();
    }

    // Measure a window of ordinary work while nine runnable threads share one core.
    const unsigned long long contended_before = cy::test::contended_ns();
    const auto started = std::chrono::steady_clock::now();
    volatile unsigned long long sink = 0;
    while (std::chrono::steady_clock::now() < started + std::chrono::milliseconds(60)) {
        sink = sink + 1U;
    }
    const auto wall = std::chrono::steady_clock::now() - started;
    const unsigned long long contended = cy::test::contended_ns() - contended_before;
    (void)sink;

    stop.store(true, std::memory_order_relaxed);
    for (std::thread& spinner : spinners) {
        spinner.join();
    }
    (void)::sched_setaffinity(0, sizeof(original), &original);

    const auto wall_ns = static_cast<unsigned long long>(std::chrono::nanoseconds(wall).count());
    CY_REQUIRE(wall_ns >= 50000000ULL);

    // THE ASSERTION: with nine runnable threads on one core, this thread spent a measurable part of
    // the window wanting a core and not having one. A quarter of the window is a low bar
    // deliberately — the measured figure is four fifths of it — because the claim is that the clock
    // MOVES under preemption, which is what lets the guard subtract it, and not that the scheduler
    // divides time in any particular way.
    CY_CHECK_GT(contended, wall_ns / 4U);
    CY_TEST_MESSAGE("contended for " << (static_cast<double>(contended) / 1e6) << " ms of a "
                                     << (static_cast<double>(wall_ns) / 1e6) << " ms window");
#endif
}
