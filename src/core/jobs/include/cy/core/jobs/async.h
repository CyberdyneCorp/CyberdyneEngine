#pragma once
// Workers never block on I/O or the GPU. Task 3.2.5.
//
// `core-jobs-and-concurrency` states the rule without qualification: "A worker thread SHALL NOT
// block waiting for file I/O, decompression performed elsewhere, network completion, or a GPU
// fence. Asynchronous I/O SHALL be submitted to the platform's asynchronous mechanism and its
// completion SHALL schedule a continuation. GPU fences SHALL be awaited through backend completion
// notification, never by busy-waiting or by a blocking wait on a worker."
//
// THE RULE HAS TWO HALVES AND BOTH ARE HERE.
//
// The half that refuses: `JobSystem::begin_blocking_region` fails on a worker thread, counts the
// violation in every configuration, and asserts where assertions are live. There is a test that
// tries to block on a worker and fails, which is the acceptance criterion stated as a test.
//
// The half that provides the alternative: `AsyncService` is the documented dedicated thread on
// which blocking is *allowed*. A worker hands it an operation and gets back a JobHandle; the
// service thread performs the blocking call; its completion releases the handle's gate, and
// anything that depended on it — a continuation task, a suspended coroutine — becomes runnable.
// The worker meanwhile went back to its deque. No worker is ever removed from the pool for the
// duration of an I/O, which is the property the whole design exists to preserve.
//
// A GPU fence is the same shape with a different signaller: `FenceSignal` is a gated job that the
// backend's completion notification releases. There is deliberately no `wait_for_fence` here to
// call by mistake.
//
// WHAT THIS IS NOT. It is not a filesystem and it is not an I/O API. `core-assets-and-io` owns
// those (task 3.3), and it will submit its reads through this service rather than reimplement it.
// What lives here is the *mechanism*: a thread where blocking is legal, and a completion that
// schedules a continuation.

#include <cy/core/jobs/job_system.h>
#include <cy/core/jobs/sync.h>
#include <cy/core/jobs/types.h>

namespace cy::jobs {

namespace detail {
struct AsyncServiceImpl;
}

/// The engine's dedicated asynchronous-work thread — the "Asset I/O" role in the thread-role table.
///
/// It runs two kinds of thing, and both exist so that a worker does not: operations that block, and
/// timers. A third dedicated thread for timers would be a thread that sleeps, and the service is
/// already awake on an interval.
class AsyncService {
public:
    /// A blocking operation, run on the service thread. It may read a file, wait on a semaphore, or
    /// call into a driver — that is the point of it. It must not submit work and then wait for it,
    /// because the service is one thread and would be waiting on itself.
    using Operation = void (*)(void* user) noexcept;

    AsyncService() noexcept;
    ~AsyncService();

    AsyncService(const AsyncService&) = delete;
    AsyncService& operator=(const AsyncService&) = delete;

    struct Config {
        /// Operations and timers in flight at once. Fixed, because growing this while a worker is
        /// submitting into it would be an allocation on the submission path.
        u32 capacity = 256;
        /// How often the service wakes when nothing has been submitted. Bounds a timer's accuracy.
        i64 poll_interval_ns = 1'000'000;  // 1 ms
    };

    /// `Config{}` cannot be a default argument here: a nested class's default member initialisers
    /// are not complete until the enclosing class is, so the overload takes the default instead.
    Status start(JobSystem& jobs) noexcept;
    Status start(JobSystem& jobs, const Config& config) noexcept;
    void stop() noexcept;
    [[nodiscard]] bool is_running() const noexcept;

    /// Run `operation` on the service thread. The returned handle completes when it has finished,
    /// so a caller waits on a job rather than on the operation, and a coroutine awaits it.
    ///
    /// `user` must outlive the operation — the service copies nothing.
    Expected<JobHandle, cy::Error> submit_blocking(Operation operation, void* user,
                                                   const char* name = "async.blocking",
                                                   Priority priority = Priority::Normal) noexcept;

    /// A handle that completes no earlier than `delay_ns` from now. The timer awaitable.
    Expected<JobHandle, cy::Error> after(i64 delay_ns, const char* name = "async.timer",
                                         Priority priority = Priority::Normal) noexcept;

    [[nodiscard]] u64 operations_completed() const noexcept;
    [[nodiscard]] u64 timers_fired() const noexcept;
    /// Submissions refused because the service's fixed table was full.
    [[nodiscard]] u64 refused() const noexcept;

private:
    detail::AsyncServiceImpl* impl_ = nullptr;
};

/// A GPU fence, or any other completion a backend notifies rather than one the engine polls.
///
/// `handle()` is a gated job: everything that needs the fence depends on it, and `signal()` — called
/// from the driver's completion notification, from any thread — releases it. There is no wait here
/// on purpose. A worker that needs a GPU result awaits the handle, which suspends a coroutine or
/// schedules a continuation; it never spins and never blocks.
class FenceSignal {
public:
    FenceSignal() noexcept = default;

    FenceSignal(const FenceSignal&) = delete;
    FenceSignal& operator=(const FenceSignal&) = delete;

    /// Arm the fence. Must be called before anything can depend on it.
    Status arm(JobSystem& jobs, const char* name = "gpu.fence",
               Priority priority = Priority::High) noexcept;

    /// The job that completes when the fence is signalled. Null before `arm`.
    [[nodiscard]] JobHandle handle() const noexcept { return handle_; }

    /// Release it. Safe from any thread, including one the engine does not own. Idempotent.
    void signal() noexcept;

    [[nodiscard]] bool is_signalled() const noexcept;

private:
    JobSystem* jobs_ = nullptr;
    JobHandle handle_;
};

}  // namespace cy::jobs
