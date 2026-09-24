// SPDX-License-Identifier: MIT
// THE GATE'S PROBE, KEPT: cases that are MEANT to trip the budget guard, run as a child process by
// test_budget_contention.cpp, which asserts on how the guard judged them.
//
// M11.c's fifth close refuted `757b3d9` with a probe like this one, linked against the tree's
// harness: a case whose own `vfork` child held it for 300 ms on an idle host was failed as
// `stalled:` before that commit and reported and passed as `contended:` after it. The probe lived
// in a scratch directory and was lost. It is here now, as a binary no CTest entry runs directly —
// every case in it fails by design — so that the harness's own suite can run it and read the
// verdict the REAL guard reached, through the real `CY_TEST_CASE`, rather than a re-enactment of
// the guard's arithmetic.
//
// It is compiled with the unit tier's budget, and the driver sets CY_TEST_BUDGET_SCALE so the
// ceiling is known exactly. Nothing here uses anything but the public test vocabulary, so the same
// file builds against the harness of any revision — which is how the regression is shown red on
// `757b3d9` and green after it.

#include <cy/test/test.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#if defined(__linux__)
#    include <sys/types.h>
#    include <sys/wait.h>
#    include <unistd.h>

#    include <ctime>
#endif

namespace {

constexpr auto kHeld = std::chrono::milliseconds(300);

}  // namespace

CY_TEST_CASE("probe: a case whose own vfork child holds it") {
#if defined(__linux__)
    // The parent of a `vfork` sleeps UNINTERRUPTIBLY until the child execs or exits, so the fourth
    // clock reads it as `D` for the whole 300 ms. Nothing else on the host is involved: this is the
    // case waiting on itself. The child only calls async-signal-safe functions before `_exit`.
    // vfork IS the subject, so posix_spawn, which the check suggests, would hide what is measured.
    timespec held{0, static_cast<long>(std::chrono::nanoseconds(kHeld).count())};
    // NOLINTNEXTLINE(bugprone-unsafe-functions,clang-analyzer-security.insecureAPI.vfork)
    const pid_t child = ::vfork();
    if (child == 0) {
        // NOLINTNEXTLINE(clang-analyzer-unix.Vfork): nanosleep is async-signal-safe
        while (::nanosleep(&held, &held) != 0) {
        }
        ::_exit(0);
    }
    CY_REQUIRE(child > 0);
    int status = 0;
    CY_CHECK(::waitpid(child, &status, 0) == child);
#else
    CY_TEST_MESSAGE("no vfork on this platform");
#endif
}

CY_TEST_CASE("probe: a case waiting on a mutex another thread holds") {
    // A futex wait is an interruptible sleep: the fourth clock reads `S`, so nothing is excused.
    std::mutex held;
    std::condition_variable locked;
    bool is_locked = false;
    std::mutex handshake;
    std::thread holder([&]() {
        const std::lock_guard<std::mutex> hold(held);
        {
            const std::lock_guard<std::mutex> tell(handshake);
            is_locked = true;
        }
        locked.notify_one();
        std::this_thread::sleep_for(kHeld);
    });
    {
        std::unique_lock<std::mutex> wait(handshake);
        locked.wait(wait, [&]() { return is_locked; });
    }
    {
        const std::lock_guard<std::mutex> take(held);
    }
    holder.join();
}

CY_TEST_CASE("probe: a case that spins") {
    // Running is not waiting: the CPU budget catches this, whatever the host is doing.
    const auto until = std::chrono::steady_clock::now() + kHeld;
    volatile unsigned long long sink = 0;
    while (std::chrono::steady_clock::now() < until) {
        sink = sink + 1U;
    }
}
