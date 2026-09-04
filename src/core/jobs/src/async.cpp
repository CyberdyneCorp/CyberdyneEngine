// The dedicated thread on which blocking is allowed, and the fence a backend signals. Task 3.2.5.
//
// One table, one mutex, one condition variable. The table is fixed-size and allocated by `start()`,
// so submitting an operation from a worker allocates nothing; a full table is reported rather than
// grown, because growing it would mean allocating on the very path this design exists to keep
// cheap.
//
// The service thread declares the AssetIo role, which is what makes blocking on it legal:
// `JobSystem::begin_blocking_region` refuses only on a thread holding the Worker role. That is the
// whole enforcement — one role check, in one place — and it is why the thread-role table in
// thread_role.h is machinery rather than documentation.

#include <cy/core/jobs/async.h>

#include "internal.h"

#include <atomic>
#include <new>

namespace cy::jobs {
namespace {

void noop_task(const TaskContext&, void*) noexcept {}

}  // namespace

namespace detail {

struct AsyncServiceImpl {
    /// One pending thing: an operation to run, a timer to fire, or both empty.
    struct Entry {
        AsyncService::Operation operation = nullptr;
        void* user = nullptr;
        JobHandle gate;
        /// When a timer becomes due. Zero for an operation, which is due immediately.
        i64 due_ns = 0;
        bool occupied = false;
    };

    JobSystem* jobs = nullptr;
    AsyncService::Config config;

    Entry* entries = nullptr;
    Mutex lock;
    ConditionVariable wake;

    Thread thread;
    std::atomic<bool> stopping{false};
    std::atomic<bool> running{false};

    std::atomic<u64> operations_completed{0};
    std::atomic<u64> timers_fired{0};
    std::atomic<u64> refused{0};

    Expected<JobHandle, cy::Error> enqueue(AsyncService::Operation operation, void* user,
                                           i64 due_ns, const char* name,
                                           Priority priority) noexcept;
    void run() noexcept;
};

Expected<JobHandle, cy::Error> AsyncServiceImpl::enqueue(AsyncService::Operation operation,
                                                         void* user, i64 due_ns, const char* name,
                                                         Priority priority) noexcept {
    JobDesc desc;
    desc.body = &noop_task;
    desc.name = name;
    desc.priority = priority;
    // The completion is a gated job. Everything downstream — a continuation task, a suspended
    // coroutine — depends on it, and the service thread releases it when the operation is done.
    desc.gated = true;
    auto gate = jobs->submit(desc);
    if (!gate) {
        return gate;
    }

    {
        ScopedLock<Mutex> held(lock);
        for (u32 i = 0; i < config.capacity; ++i) {
            if (entries[i].occupied) {
                continue;
            }
            entries[i].operation = operation;
            entries[i].user = user;
            entries[i].gate = gate.value();
            entries[i].due_ns = due_ns;
            entries[i].occupied = true;
            wake.notify_one();
            return gate.value();
        }
    }

    refused.fetch_add(1, std::memory_order_relaxed);
    // Release the gate: a handle nobody will ever complete is worse than a refusal, because the
    // caller would wait on it forever rather than being told.
    (void)jobs->signal(gate.value());
    return fail(ErrorCode::Unavailable,
                "the async service's table is full; raise AsyncService::Config::capacity, which is "
                "the documented bound on operations and timers in flight");
}

void AsyncServiceImpl::run() noexcept {
    // AssetIo, not Worker. This is the role that makes a blocking call on this thread legal, and it
    // is the only difference between this thread and a worker as far as the rule is concerned.
    set_thread_role(ThreadRole::AssetIo);
    running.store(true, std::memory_order_release);

    while (!stopping.load(std::memory_order_acquire)) {
        AsyncService::Operation operation = nullptr;
        void* user = nullptr;
        JobHandle gate;
        bool timer = false;

        {
            ScopedLock<Mutex> held(lock);
            const i64 now = monotonic_now_ns();
            for (u32 i = 0; i < config.capacity; ++i) {
                if (!entries[i].occupied || entries[i].due_ns > now) {
                    continue;
                }
                operation = entries[i].operation;
                user = entries[i].user;
                gate = entries[i].gate;
                timer = entries[i].operation == nullptr;
                entries[i].occupied = false;
                break;
            }
        }

        if (gate.is_null()) {
            lock.lock();
            // A timed wait: a timer that is not yet due has no notification to wait for, so the
            // poll interval is what bounds its accuracy.
            wake.wait_for(lock, config.poll_interval_ns,
                          [this] { return stopping.load(std::memory_order_acquire); });
            lock.unlock();
            continue;
        }

        if (operation != nullptr) {
            operation(user);
            operations_completed.fetch_add(1, std::memory_order_relaxed);
        }
        if (timer) {
            timers_fired.fetch_add(1, std::memory_order_relaxed);
        }
        (void)jobs->signal(gate);
    }

    // Everything still pending is released rather than abandoned: a caller waiting on a handle this
    // service will never complete would wait for the life of the process.
    ScopedLock<Mutex> held(lock);
    for (u32 i = 0; i < config.capacity; ++i) {
        if (entries[i].occupied) {
            entries[i].occupied = false;
            (void)jobs->signal(entries[i].gate);
        }
    }
    running.store(false, std::memory_order_release);
}

}  // namespace detail

AsyncService::AsyncService() noexcept = default;

AsyncService::~AsyncService() {
    stop();
}

Status AsyncService::start(JobSystem& jobs) noexcept {
    return start(jobs, Config{});
}

Status AsyncService::start(JobSystem& jobs, const Config& configuration) noexcept {
    if (impl_ != nullptr) {
        return fail(ErrorCode::AlreadyExists, "this async service is already running");
    }
    if (!jobs.is_running()) {
        return fail(ErrorCode::Unavailable,
                    "the async service publishes its completions as job handles, so the job system "
                    "must be running first");
    }
    if (configuration.capacity == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "an async service with no capacity accepts nothing");
    }

    auto* impl = new (std::nothrow) detail::AsyncServiceImpl();
    if (impl == nullptr) {
        return fail(ErrorCode::OutOfMemory, "the async service could not be allocated");
    }
    impl->jobs = &jobs;
    impl->config = configuration;
    impl->entries = new (std::nothrow) detail::AsyncServiceImpl::Entry[configuration.capacity];
    if (impl->entries == nullptr) {
        delete impl;
        return fail(ErrorCode::OutOfMemory, "the async service's table could not be allocated");
    }

    impl->thread = Thread("cy.jobs.async", [impl] { impl->run(); });
    impl_ = impl;
    return ok();
}

void AsyncService::stop() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    impl_->stopping.store(true, std::memory_order_release);
    impl_->wake.notify_all();
    impl_->thread.join();
    delete[] impl_->entries;
    delete impl_;
    impl_ = nullptr;
}

bool AsyncService::is_running() const noexcept {
    return impl_ != nullptr && impl_->running.load(std::memory_order_acquire);
}

Expected<JobHandle, cy::Error> AsyncService::submit_blocking(Operation operation, void* user,
                                                             const char* name,
                                                             Priority priority) noexcept {
    if (impl_ == nullptr) {
        return fail(ErrorCode::Unavailable, "the async service is not running");
    }
    if (operation == nullptr) {
        return fail(ErrorCode::InvalidArgument, "an asynchronous operation needs a body");
    }
    return impl_->enqueue(operation, user, 0, name, priority);
}

Expected<JobHandle, cy::Error> AsyncService::after(i64 delay_ns, const char* name,
                                                   Priority priority) noexcept {
    if (impl_ == nullptr) {
        return fail(ErrorCode::Unavailable, "the async service is not running");
    }
    const i64 due = monotonic_now_ns() + (delay_ns > 0 ? delay_ns : 0);
    return impl_->enqueue(nullptr, nullptr, due, name, priority);
}

u64 AsyncService::operations_completed() const noexcept {
    return impl_ != nullptr ? impl_->operations_completed.load(std::memory_order_relaxed) : 0;
}

u64 AsyncService::timers_fired() const noexcept {
    return impl_ != nullptr ? impl_->timers_fired.load(std::memory_order_relaxed) : 0;
}

u64 AsyncService::refused() const noexcept {
    return impl_ != nullptr ? impl_->refused.load(std::memory_order_relaxed) : 0;
}

// --- FenceSignal
// ----------------------------------------------------------------------------------

Status FenceSignal::arm(JobSystem& jobs, const char* name, Priority priority) noexcept {
    if (!handle_.is_null()) {
        return fail(ErrorCode::AlreadyExists, "this fence is already armed");
    }
    JobDesc desc;
    desc.body = &noop_task;
    desc.name = name;
    desc.priority = priority;
    desc.gated = true;
    auto gate = jobs.submit(desc);
    if (!gate) {
        return fail(gate.error().code, gate.error().message, gate.error().system_code);
    }
    jobs_ = &jobs;
    handle_ = gate.value();
    return ok();
}

void FenceSignal::signal() noexcept {
    if (jobs_ != nullptr && !handle_.is_null()) {
        (void)jobs_->signal(handle_);
    }
}

bool FenceSignal::is_signalled() const noexcept {
    return jobs_ != nullptr && !handle_.is_null() && jobs_->is_complete(handle_);
}

}  // namespace cy::jobs
