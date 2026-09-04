// The command queue: serialise a call, replay it on the thread that owns the state.
//
// The arena is a bump buffer reset by every drain, which is the only allocation discipline that
// makes sense here: a command's arguments are needed exactly until the owner has run it, every
// command between two drains has the same lifetime, and freeing them individually would be work
// done for no reason. A drain therefore costs one store to reset the arena.
//
// A drain copies the record array out under the lock and runs the handlers outside it. A handler
// runs arbitrary subsystem code — it records render commands, it touches audio state — and holding
// the submission lock across that would make every producer wait for the owner's work rather than
// for the queue.

#include <cy/core/jobs/command_queue.h>

#include <cy/core/base/assert.h>
#include <cy/core/jobs/diagnostics.h>
#include <cy/core/jobs/job_system.h>

#include <atomic>
#include <cstring>
#include <new>

namespace cy::jobs {
namespace {

/// A token identifying the calling thread. A counter rather than std::thread::id because the value
/// is stored in an atomic and compared, and a std::thread::id is not required to fit one.
u64 current_thread_token() noexcept {
    static std::atomic<u64> next{1};
    thread_local const u64 token = next.fetch_add(1, std::memory_order_relaxed);
    return token;
}

}  // namespace

CommandQueue::~CommandQueue() {
    delete[] records_;
    delete[] staging_;
    delete[] arena_;
    delete[] registry_;
    delete[] completions_;
    delete[] completion_free_;
    records_ = nullptr;
    staging_ = nullptr;
    arena_ = nullptr;
    registry_ = nullptr;
    completions_ = nullptr;
    completion_free_ = nullptr;
}

Status CommandQueue::initialize(const Config& configuration) noexcept {
    if (configuration.capacity == 0 || configuration.max_commands == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "a command queue needs room for at least one command and one registration");
    }
    config_ = configuration;

    records_ = new (std::nothrow) Record[config_.capacity];
    staging_ = new (std::nothrow) Record[config_.capacity];
    registry_ = new (std::nothrow) Registration[config_.max_commands];
    completions_ = new (std::nothrow) Event[config_.capacity];
    completion_free_ = new (std::nothrow) u32[config_.capacity];
    arena_ = config_.arena_bytes != 0 ? new (std::nothrow) u8[config_.arena_bytes] : nullptr;

    if (records_ == nullptr || staging_ == nullptr || registry_ == nullptr ||
        completions_ == nullptr || completion_free_ == nullptr ||
        (config_.arena_bytes != 0 && arena_ == nullptr)) {
        return fail(ErrorCode::OutOfMemory, "the command queue could not be allocated");
    }
    for (u32 i = 0; i < config_.capacity; ++i) {
        completion_free_[i] = i;
    }
    completion_free_count_ = config_.capacity;
    return ok();
}

Status CommandQueue::register_command(CommandId id, CommandHandler handler, void* user,
                                      const char* name) noexcept {
    if (registry_ == nullptr) {
        return fail(ErrorCode::Unavailable, "this command queue was never initialised");
    }
    if (handler == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a command needs a handler");
    }

    ScopedLock<Mutex> held(lock_);
    for (u32 i = 0; i < config_.max_commands; ++i) {
        if (registry_[i].used && registry_[i].id == id) {
            return fail(ErrorCode::AlreadyExists, "this command identifier is already registered");
        }
    }
    for (u32 i = 0; i < config_.max_commands; ++i) {
        if (!registry_[i].used) {
            registry_[i].used = true;
            registry_[i].id = id;
            registry_[i].handler = handler;
            registry_[i].user = user;
            registry_[i].name = name != nullptr ? name : "command";
            return ok();
        }
    }
    return fail(ErrorCode::OutOfRange,
                "this command queue's registration table is full; raise Config::max_commands");
}

const CommandQueue::Registration* CommandQueue::find(CommandId id) const noexcept {
    for (u32 i = 0; i < config_.max_commands; ++i) {
        if (registry_[i].used && registry_[i].id == id) {
            return &registry_[i];
        }
    }
    return nullptr;
}

u32 CommandQueue::claim_completion() noexcept {
    ScopedLock<Mutex> held(lock_);
    if (completion_free_count_ == 0) {
        return kNoCompletion;
    }
    return completion_free_[--completion_free_count_];
}

void CommandQueue::release_completion(u32 slot) noexcept {
    if (slot == kNoCompletion) {
        return;
    }
    ScopedLock<Mutex> held(lock_);
    completion_free_[completion_free_count_++] = slot;
}

Status CommandQueue::enqueue(CommandId id, const void* arguments, usize argument_bytes,
                             void* result, usize result_bytes, u32 completion) noexcept {
    if (records_ == nullptr) {
        return fail(ErrorCode::Unavailable, "this command queue was never initialised");
    }
    if (argument_bytes != 0 && arguments == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a command with a size needs arguments");
    }

    ScopedLock<Mutex> held(lock_);
    if (find(id) == nullptr) {
        return fail(ErrorCode::NotFound,
                    "no handler is registered for this command; the owning subsystem registers "
                    "its commands before any caller submits one");
    }
    if (count_ == config_.capacity) {
        refused_.fetch_add(1);
        return fail(ErrorCode::OutOfRange,
                    "this command queue is full; the owning thread has not drained it, or "
                    "Config::capacity is below the frame's worst case");
    }

    Record& record = records_[count_];
    record.id = id;
    record.argument_bytes = static_cast<u32>(argument_bytes);
    record.result = result;
    record.result_bytes = result_bytes;
    record.completion = completion;
    record.arena_offset = 0xFFFF'FFFFu;

    if (argument_bytes <= kInlineArgumentBytes) {
        if (argument_bytes != 0) {
            std::memcpy(record.inline_arguments, arguments, argument_bytes);
        }
    } else {
        // The arena, not the heap. Aligned to sixteen so that a payload with any alignment the
        // engine uses lands correctly.
        const usize aligned = (arena_used_ + 15) & ~usize{15};
        if (arena_ == nullptr || aligned > config_.arena_bytes ||
            argument_bytes > config_.arena_bytes - aligned) {
            refused_.fetch_add(1);
            return fail(ErrorCode::OutOfRange,
                        "this command's arguments do not fit the queue's arena; raise "
                        "Config::arena_bytes rather than moving the payload to the heap");
        }
        std::memcpy(arena_ + aligned, arguments, argument_bytes);
        record.arena_offset = static_cast<u32>(aligned);
        arena_used_ = aligned + argument_bytes;
    }

    ++count_;
    submitted_.fetch_add(1);
    return ok();
}

Status CommandQueue::submit(CommandId id, const void* arguments, usize argument_bytes) noexcept {
    return enqueue(id, arguments, argument_bytes, nullptr, 0, kNoCompletion);
}

Status CommandQueue::submit_sync(CommandId id, const void* arguments, usize argument_bytes,
                                 void* result, usize result_bytes) noexcept {
    // The owning thread drains its own queue rather than waiting for itself. Without this a server
    // that used its own queue would deadlock on its first synchronous call.
    const u64 me = current_thread_token();
    const bool is_owner = owner_.load() == me;

    synchronous_.fetch_add(1);
    const u64 this_frame = synchronous_this_frame_.fetch_add(1) + 1;
    if (this_frame > config_.synchronous_warning_threshold) {
        const Registration* registration = nullptr;
        {
            ScopedLock<Mutex> held(lock_);
            registration = find(id);
        }
        // The specification's "a synchronous cross-thread call made every frame in a development
        // build SHALL be identified". Reported rather than asserted, because it is a design smell
        // rather than a defect, and because a report survives Profile and Shipping.
        jobs_log_watchdog("synchronous-command",
                          registration != nullptr ? registration->name : "(unregistered)",
                          this_frame, kNotAWorker);
    }

    if (is_owner) {
        if (auto status =
                enqueue(id, arguments, argument_bytes, result, result_bytes, kNoCompletion);
            !status) {
            return status;
        }
        drain();
        return ok();
    }

    // Off the owner's thread this blocks. On a job worker that is the same defect as blocking on a
    // disk: the pool loses a thread until another thread gets round to draining. Refused, counted,
    // and asserted where assertions are live — the same one mechanism as every other blocking call.
    JobSystem* jobs = JobSystem::current();
    if (jobs != nullptr) {
        if (auto permitted = jobs->begin_blocking_region("CommandQueue::submit_sync"); !permitted) {
            return permitted;
        }
    }

    // A pooled event, owned by the queue. See CommandQueue::Record::completion: one on this
    // thread's stack would be destroyed while the owner thread is still inside the notification.
    const u32 slot = claim_completion();
    if (slot == kNoCompletion) {
        if (jobs != nullptr) {
            jobs->end_blocking_region();
        }
        return fail(ErrorCode::Unavailable,
                    "every completion slot is in use; there cannot be more synchronous calls "
                    "outstanding than Config::capacity");
    }
    completions_[slot].reset();

    Status enqueued = enqueue(id, arguments, argument_bytes, result, result_bytes, slot);
    if (enqueued) {
        completions_[slot].wait();
    }
    release_completion(slot);
    if (jobs != nullptr) {
        jobs->end_blocking_region();
    }
    return enqueued;
}

void CommandQueue::execute(const Record& record) noexcept {
    // The registration is copied out under the lock rather than dereferenced through it: the
    // handler runs arbitrary subsystem code, and holding the submission lock across that would make
    // every producer wait for the owner's work rather than for the queue.
    Registration registration;
    {
        ScopedLock<Mutex> held(lock_);
        const Registration* found = find(record.id);
        if (found == nullptr) {
            return;
        }
        registration = *found;
    }
    const void* arguments = record.arena_offset == 0xFFFF'FFFFu
                                ? static_cast<const void*>(record.inline_arguments)
                                : static_cast<const void*>(arena_ + record.arena_offset);
    registration.handler(arguments, record.argument_bytes, record.result, record.result_bytes,
                         registration.user);
}

u64 CommandQueue::drain() noexcept {
    if (records_ == nullptr) {
        return 0;
    }
    claim_owner();

    u32 count = 0;
    {
        ScopedLock<Mutex> held(lock_);
        count = count_;
        for (u32 i = 0; i < count; ++i) {
            staging_[i] = records_[i];
        }
        count_ = 0;
        // The arena is reset here rather than after the handlers run, and the staged records still
        // point into it. That is safe because a producer cannot write into it while this thread
        // holds the lock, and the next producer to take the lock begins at offset zero — by which
        // time the handlers below have already read what they needed.
        //
        // It is also the one thing in this file that would break if a drain ran concurrently with
        // another drain, which is why the owner is a single thread and `claim_owner` records it.
        arena_used_ = 0;
    }

    for (u32 i = 0; i < count; ++i) {
        execute(staging_[i]);
        executed_.fetch_add(1);
        if (staging_[i].completion != kNoCompletion) {
            completions_[staging_[i].completion].set();
        }
    }
    return count;
}

void CommandQueue::claim_owner() noexcept {
    u64 expected = 0;
    (void)owner_.compare_exchange_strong(expected, current_thread_token());
}

void CommandQueue::begin_frame() noexcept {
    synchronous_this_frame_.store(0);
}

u32 CommandQueue::pending() const noexcept {
    ScopedLock<Mutex> held(lock_);
    return count_;
}

u64 CommandQueue::submitted() const noexcept {
    return submitted_.load();
}

u64 CommandQueue::executed() const noexcept {
    return executed_.load();
}

u64 CommandQueue::synchronous_calls() const noexcept {
    return synchronous_.load();
}

u64 CommandQueue::synchronous_calls_this_frame() const noexcept {
    return synchronous_this_frame_.load();
}

u64 CommandQueue::refused() const noexcept {
    return refused_.load();
}

}  // namespace cy::jobs
