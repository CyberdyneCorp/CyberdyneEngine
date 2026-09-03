#pragma once
// Coroutines are the asynchronous model. Task 3.2.4.
//
// `core-jobs-and-concurrency`: "Asynchronous engine work SHALL be expressed with C++20 coroutines
// whose continuations the job system resumes as ordinary tasks. A coroutine SHALL be able to await:
// another job or coroutine, an asynchronous I/O completion, a GPU fence, a task group, a timer, and
// a cancellation. A fiber runtime is a non-goal."
//
// WHAT A `co_await` DOES HERE, because it is the whole design in one paragraph. The awaiter asks
// whether the thing is already done; if it is, the coroutine does not suspend at all. If it is not,
// the awaiter registers a *continuation job* — an ordinary task, with the awaiting coroutine's
// priority — and returns. The worker then goes back to its deque and runs something else. When the
// awaited thing completes, its completion makes the continuation runnable, and the coroutine picks
// up on whichever worker takes it. Nothing waits, nothing spins, and no worker is removed from the
// pool for the duration of an I/O.
//
// A FIBER RUNTIME IS A NON-GOAL, and this is what that means concretely: there is no stack
// switching anywhere in this file. A suspended coroutine is a heap frame and a resume pointer, and
// the worker's own stack is unwound back to the scheduler loop before anything else runs on it.
//
// FRAMES COME FROM PER-WORKER SLABS. `promise_type::operator new` takes the frame from a
// thread-local free list of size classes, so a coroutine that suspends and resumes a thousand times
// a frame does not call the general allocator a thousand times. A frame larger than the largest
// size class falls back to the general heap and is counted; `coroutine_frame_stats()` reports both,
// so "awaiting does not allocate on the common path" is a number rather than a claim.
//
// -fno-exceptions. `unhandled_exception()` is required by the language and cannot be reached: the
// engine compiles with exceptions off, so no coroutine body can throw. Allocation failure is
// reported through `get_return_object_on_allocation_failure`, which yields an invalid Task —
// checked with `is_valid()` — rather than through a bad_alloc nobody could catch.

#include <cy/core/jobs/cancellation.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/jobs/types.h>

#include <coroutine>
#include <new>
#include <type_traits>
#include <utility>

namespace cy::jobs {

/// Where a coroutine frame came from. Both numbers are cumulative for the process.
struct CoroutineFrameStats {
    /// Frames taken from a thread-local slab: the common path.
    u64 from_slab = 0;
    /// Frames too large for the largest size class, taken from the general heap.
    u64 from_heap = 0;
    /// Frames currently alive.
    u64 live = 0;
    /// Bytes held by the slabs' free lists across every thread that has used one.
    u64 slab_bytes = 0;
};

CoroutineFrameStats coroutine_frame_stats() noexcept;

namespace detail {

/// The frame allocator behind `promise_type::operator new`. Thread-local free lists of fixed size
/// classes; see the header comment.
void* coroutine_frame_allocate(usize bytes) noexcept;
void coroutine_frame_free(void* frame, usize bytes) noexcept;

/// The body of every continuation job: resume the coroutine whose handle travelled in the task
/// record's inline arguments.
void resume_coroutine(const TaskContext& context, void* user) noexcept;

/// What a coroutine signals when it finishes, so that whoever spawned it can wait on a JobHandle
/// rather than on the coroutine itself.
struct Completion {
    JobSystem* jobs = nullptr;
    JobHandle handle;

    void signal() const noexcept {
        if (jobs != nullptr && !handle.is_null()) {
            (void)jobs->signal(handle);
        }
    }
};

/// Everything a promise needs that does not depend on the coroutine's return type.
class TaskPromiseBase {
public:
    /// Lazy: a Task does not start until it is awaited or spawned. An eager coroutine would run on
    /// the thread that created it, which is exactly the thread a caller usually wanted to keep.
    std::suspend_always initial_suspend() noexcept { return {}; }

    /// Symmetric transfer into the awaiting coroutine, or the completion signal. Returning the
    /// continuation's handle rather than resuming it means the two coroutines share one stack frame
    /// depth: a chain of a thousand awaits does not build a stack a thousand deep.
    ///
    /// THE ORDER OF THE TWO LINES BELOW IS A LIFETIME RULE, not a preference. Releasing the
    /// completion gate is what lets whoever spawned this coroutine return from its wait — and the
    /// next thing that owner does is destroy the Task, which frees this very frame. Anything read
    /// out of the promise *after* the signal is read out of freed memory. So the continuation is
    /// taken first, and the signal is the last thing that touches the frame.
    ///
    /// The two are also mutually exclusive in practice: a coroutine that was co_awaited has a
    /// continuation and no completion, and one that was spawned has a completion and no
    /// continuation. Handling them as an either/or keeps the signal out of the awaited path
    /// entirely.
    struct FinalAwaiter {
        [[nodiscard]] bool await_ready() const noexcept { return false; }

        template <class Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> self) noexcept {
            TaskPromiseBase& promise = self.promise();
            const std::coroutine_handle<> continuation = promise.continuation_;
            if (continuation != nullptr) {
                return continuation;
            }
            const Completion completion = promise.completion_;
            completion.signal();
            return std::noop_coroutine();
        }

        void await_resume() const noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return {}; }

    /// Unreachable: the engine compiles with -fno-exceptions, so nothing in a coroutine body can
    /// throw. The language requires the member to exist.
    void unhandled_exception() noexcept {}

    void set_continuation(std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;
    }
    void set_completion(const Completion& completion) noexcept { completion_ = completion; }

    [[nodiscard]] Priority priority() const noexcept { return priority_; }
    void set_priority(Priority priority) noexcept { priority_ = priority; }

    static void* operator new(usize size) noexcept { return coroutine_frame_allocate(size); }
    static void operator delete(void* frame, usize size) noexcept {
        coroutine_frame_free(frame, size);
    }

private:
    std::coroutine_handle<> continuation_ = nullptr;
    Completion completion_;
    Priority priority_ = Priority::Normal;
};

}  // namespace detail

/// A coroutine that produces a `T`.
///
/// Lazy and move-only. Awaiting one from another coroutine chains them with symmetric transfer;
/// `co_spawn` starts one from ordinary code and hands back a JobHandle that completes when it does.
template <class T = void>
class Task {
public:
    class promise_type : public detail::TaskPromiseBase {
    public:
        Task get_return_object() noexcept {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        /// The -fno-exceptions path for a frame that could not be allocated: an invalid Task, which
        /// the caller checks with `is_valid()`.
        static Task get_return_object_on_allocation_failure() noexcept { return Task(); }

        template <class U>
        void return_value(U&& value) noexcept {
            value_ = std::forward<U>(value);
        }

        [[nodiscard]] T& value() noexcept { return value_; }

    private:
        T value_{};
    };

    using Handle = std::coroutine_handle<promise_type>;

    Task() noexcept = default;
    explicit Task(Handle handle) noexcept : handle_(handle) {}

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }
    ~Task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    /// False when the frame could not be allocated. Every other member is meaningless then.
    [[nodiscard]] bool is_valid() const noexcept { return static_cast<bool>(handle_); }
    [[nodiscard]] bool done() const noexcept { return handle_ && handle_.done(); }
    [[nodiscard]] Handle handle() const noexcept { return handle_; }
    [[nodiscard]] T& result() noexcept { return handle_.promise().value(); }

    /// Await from another coroutine. Symmetric transfer: the awaiting coroutine's handle becomes
    /// this one's continuation, and control passes to this one without returning to the scheduler.
    auto operator co_await() noexcept {
        struct Awaiter {
            Handle inner;
            [[nodiscard]] bool await_ready() const noexcept { return !inner || inner.done(); }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
                inner.promise().set_continuation(awaiting);
                return inner;
            }
            T& await_resume() const noexcept { return inner.promise().value(); }
        };
        return Awaiter{handle_};
    }

private:
    Handle handle_{};
};

/// The void specialisation. Written out rather than reached through a `Void` placeholder, because
/// `co_return;` and `co_return value;` are different members of the promise and the language will
/// not let one stand for the other.
template <>
class Task<void> {
public:
    class promise_type : public detail::TaskPromiseBase {
    public:
        Task get_return_object() noexcept {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        static Task get_return_object_on_allocation_failure() noexcept { return Task(); }
        void return_void() noexcept {}
    };

    using Handle = std::coroutine_handle<promise_type>;

    Task() noexcept = default;
    explicit Task(Handle handle) noexcept : handle_(handle) {}

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }
    ~Task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    [[nodiscard]] bool is_valid() const noexcept { return static_cast<bool>(handle_); }
    [[nodiscard]] bool done() const noexcept { return handle_ && handle_.done(); }
    [[nodiscard]] Handle handle() const noexcept { return handle_; }

    auto operator co_await() noexcept {
        struct Awaiter {
            Handle inner;
            [[nodiscard]] bool await_ready() const noexcept { return !inner || inner.done(); }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
                inner.promise().set_continuation(awaiting);
                return inner;
            }
            void await_resume() const noexcept {}
        };
        return Awaiter{handle_};
    }

private:
    Handle handle_{};
};

// --- Awaiting a job ------------------------------------------------------------------------------

/// Awaiting a JobHandle: the continuation is an ordinary task with the awaiting coroutine's
/// priority, scheduled when the job completes and free to run on a different worker.
///
/// This one awaiter covers most of the specification's list. A task group is the JobHandle
/// `submit_parallel_for` returns; an asynchronous I/O completion and a GPU fence are gated jobs
/// (async.h) that their completion signals; a timer is a gated job the async service releases.
class JobAwaiter {
public:
    JobAwaiter(JobSystem& jobs, JobHandle handle, Priority priority) noexcept
        : jobs_(&jobs), handle_(handle), priority_(priority) {}

    [[nodiscard]] bool await_ready() const noexcept { return jobs_->is_complete(handle_); }

    /// Returns false when the continuation could not be scheduled, which resumes the coroutine
    /// immediately rather than suspending it forever. The wait that then happens is the helping
    /// kind — the thread runs other ready tasks — so it is a slower path, not a stalled one.
    bool await_suspend(std::coroutine_handle<> awaiting) noexcept {
        JobDesc desc;
        desc.body = &detail::resume_coroutine;
        desc.name = "coroutine.continuation";
        desc.priority = priority_;
        desc.dependencies = &handle_;
        desc.dependency_count = 1;
        const void* address = awaiting.address();
        desc.inline_data = &address;
        desc.inline_size = static_cast<u32>(sizeof(address));

        if (auto submitted = jobs_->submit(desc); submitted) {
            return true;
        }
        jobs_->wait(handle_);
        return false;
    }

    void await_resume() const noexcept {}

private:
    JobSystem* jobs_;
    JobHandle handle_;
    Priority priority_;
};

/// `co_await await_job(jobs, handle);`
inline JobAwaiter await_job(JobSystem& jobs, JobHandle handle,
                            Priority priority = Priority::Normal) noexcept {
    return JobAwaiter(jobs, handle, priority);
}

/// Awaiting a cancellation. Resumes when the token is cancelled; if it already is, the coroutine
/// never suspends. Used to race an operation against its own cancellation.
class CancellationAwaiter {
public:
    CancellationAwaiter(JobSystem& jobs, CancellationToken token, Priority priority) noexcept
        : jobs_(&jobs), token_(std::move(token)), priority_(priority) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return !token_.can_be_cancelled() || token_.is_cancelled();
    }

    bool await_suspend(std::coroutine_handle<> awaiting) noexcept;

    void await_resume() const noexcept {}

private:
    JobSystem* jobs_;
    CancellationToken token_;
    Priority priority_;
    JobHandle gate_;
};

inline CancellationAwaiter await_cancellation(JobSystem& jobs, CancellationToken token,
                                              Priority priority = Priority::Normal) noexcept {
    return CancellationAwaiter(jobs, std::move(token), priority);
}

// --- Starting a coroutine from ordinary code -----------------------------------------------------

namespace detail {

/// Shared by both `co_spawn` overloads: create the gated completion job, attach it to the promise,
/// and submit the first resumption.
Expected<JobHandle, cy::Error> spawn_coroutine(JobSystem& jobs, std::coroutine_handle<> coroutine,
                                               TaskPromiseBase& promise, const char* name,
                                               Priority priority) noexcept;

}  // namespace detail

/// Start a coroutine and get a JobHandle that completes when it finishes.
///
/// The caller owns the Task and must keep it alive until the returned handle completes — the
/// coroutine frame lives in it. That is deliberate: the alternative, a detached coroutine that owns
/// itself, needs an allocation per spawn and leaves nobody to ask for the result.
template <class T>
Expected<JobHandle, cy::Error> co_spawn(JobSystem& jobs, Task<T>& task,
                                        const char* name = "coroutine",
                                        Priority priority = Priority::Normal) noexcept {
    if (!task.is_valid()) {
        return fail(ErrorCode::OutOfMemory,
                    "this coroutine's frame could not be allocated, so there is nothing to start");
    }
    return detail::spawn_coroutine(jobs, task.handle(), task.handle().promise(), name, priority);
}

/// Run a coroutine to completion from ordinary code, helping with other work while it is suspended.
/// The blocking form, for a frame boundary or a test — not for a worker in the middle of a stage.
template <class T>
Status sync_await(JobSystem& jobs, Task<T>& task, const char* name = "coroutine",
                  Priority priority = Priority::Normal) noexcept {
    auto handle = co_spawn(jobs, task, name, priority);
    if (!handle) {
        return fail(handle.error().code, handle.error().message, handle.error().system_code);
    }
    jobs.wait(handle.value());
    return ok();
}

}  // namespace cy::jobs
