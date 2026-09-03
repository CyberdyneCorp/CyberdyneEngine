#pragma once
// What every jobs suite needs: a job system that is started and stopped exactly once, and the
// profile question every assertion about an assertion has to ask first.

#include <cy/core/jobs/job_system.h>
#include <cy/test/test.h>

namespace cy::jobs::test {

/// True where CY_ASSERT is live: Debug and Development. False in Profile and Shipping.
///
/// A test that asserts on *assertion behaviour* must check this first. M0's suite went red in two
/// profiles for exactly this reason, and it is why every check in this module is counted as well as
/// asserted — the counters are live in all four configurations, so the interesting half of each
/// test runs everywhere and only the assertion half is conditional.
constexpr bool assertions_are_live() noexcept {
#if defined(CY_DEVELOPMENT)
    return true;
#else
    return false;
#endif
}

/// A job system for the duration of a scope.
///
/// `core-jobs-and-concurrency` allows exactly one running system per process, so a suite that
/// started two would fail its second `start()` rather than testing anything. This makes the
/// lifetime a scope, and doctest runs test cases one at a time, so the systems never overlap.
class ScopedJobSystem {
public:
    explicit ScopedJobSystem(const JobSystemConfig& config) noexcept {
        started_ = system_.start(config).has_value();
    }

    /// The common shape: a fixed worker count, so a test's expectations do not depend on the
    /// machine it runs on.
    explicit ScopedJobSystem(u32 workers = 4) noexcept {
        JobSystemConfig config;
        config.worker_count = workers;
        config.task_slots_per_participant = 2048;
        config.deque_capacity = 2048;
        config.scratch_bytes_per_participant = 256 * 1024;
        started_ = system_.start(config).has_value();
    }

    ~ScopedJobSystem() { system_.shutdown(); }

    ScopedJobSystem(const ScopedJobSystem&) = delete;
    ScopedJobSystem& operator=(const ScopedJobSystem&) = delete;

    [[nodiscard]] bool started() const noexcept { return started_; }
    JobSystem& operator*() noexcept { return system_; }
    JobSystem* operator->() noexcept { return &system_; }
    [[nodiscard]] JobSystem& get() noexcept { return system_; }

private:
    JobSystem system_;
    bool started_ = false;
};

}  // namespace cy::jobs::test
