#pragma once
// The command queue for single-threaded servers. Task 3.2.9.
//
// `core-jobs-and-concurrency`: "`CommandQueue` SHALL serialise calls into a byte buffer for replay
// on an owning thread, supporting fire-and-forget submission and synchronous submission that blocks
// until the command has executed and a result is written back. Commands SHALL store their arguments
// by value and be trivially relocatable; a command exceeding the inline size limit SHALL allocate
// from the queue's arena rather than the heap."
//
// WHAT IT IS FOR. Several subsystems own a thread and nothing else may touch their state — the
// render thread owns every RHI object, the audio thread owns playback state. A caller on another
// thread does not reach in; it enqueues a call, and the owner replays it at a point of its
// choosing. That is the whole mechanism, and it is why the thread-role table can be enforced rather
// than merely written down.
//
// ARGUMENTS ARE COPIED BY VALUE, and must be trivially copyable: a record is memcpy'd into the
// queue and memcpy'd back out on the owner's thread, and nothing between the two runs a
// constructor. Up to `kInlineArgumentBytes` they live in the record; beyond that they go into the
// queue's arena, which is a bump buffer reset by each drain — never the general heap, which is what
// the specification means by "from the queue's arena rather than the heap".
//
// THE SYNCHRONOUS PATH IS DELIBERATELY UNCOMFORTABLE. It serialises the frame: the caller stops
// until the owner reaches its next drain. Two things follow, and both are enforced rather than
// advised. On a job worker it is refused outright — a worker that waits for another thread is the
// same defect as a worker that waits for a disk, and `JobSystem::begin_blocking_region` reports it
// the same way. Everywhere else, a development build counts synchronous calls per frame and warns,
// naming the command, which is the specification's "a synchronous cross-thread call made every
// frame in a development build SHALL be identified".

#include <cy/core/jobs/sync.h>
#include <cy/core/jobs/types.h>

namespace cy::jobs {

/// A registered command. The number is the caller's to choose and is not derived from anything;
/// a server declares its own enumeration and casts.
using CommandId = u32;

/// What the owning thread runs when it drains a command.
///
/// `arguments` is the caller's payload, copied by value. `result` is the caller's storage on a
/// synchronous submission and null otherwise; a handler that writes it must respect
/// `result_bytes`. `user` is whatever was registered with the command.
using CommandHandler = void (*)(const void* arguments, usize argument_bytes, void* result,
                                usize result_bytes, void* user) noexcept;

/// Arguments up to this size travel inside the record; anything larger goes to the arena.
inline constexpr usize kInlineArgumentBytes = 64;

class CommandQueue {
public:
    struct Config {
        /// Commands the queue holds between drains.
        u32 capacity = 1024;
        /// Bytes for arguments too large to be inline. Reset by every drain.
        usize arena_bytes = usize{64} * 1024;
        /// Distinct command identifiers that may be registered.
        u32 max_commands = 64;
        /// Synchronous calls per frame beyond which a development build warns. One is a
        /// handover; sixty a second is a serialised frame.
        u32 synchronous_warning_threshold = 1;
    };

    CommandQueue() noexcept = default;
    ~CommandQueue();

    CommandQueue(const CommandQueue&) = delete;
    CommandQueue& operator=(const CommandQueue&) = delete;

    Status initialize(const Config& config) noexcept;

    /// Register a handler. Called by the owning subsystem before any caller submits.
    Status register_command(CommandId id, CommandHandler handler, void* user,
                            const char* name) noexcept;

    /// Enqueue and return immediately. The call runs on the owner's thread at its next drain.
    Status submit(CommandId id, const void* arguments, usize argument_bytes) noexcept;

    /// Enqueue and wait until the owner has run it and written the result back.
    ///
    /// Refused on a job worker: see the header. Called on the owning thread itself it drains
    /// inline rather than deadlocking, which is what makes a server able to use its own queue.
    Status submit_sync(CommandId id, const void* arguments, usize argument_bytes, void* result,
                       usize result_bytes) noexcept;

    /// Run every enqueued command, in submission order, on the calling thread. Returns how many
    /// ran. The calling thread becomes the queue's owner if none has claimed it.
    u64 drain() noexcept;

    /// Declare the calling thread the owner. Optional — `drain()` does it — but a server that wants
    /// `submit_sync` from its own thread to drain inline before its first drain calls it at start.
    void claim_owner() noexcept;

    /// Open a frame: resets the synchronous-call count the warning is measured against.
    void begin_frame() noexcept;

    [[nodiscard]] u32 pending() const noexcept;
    [[nodiscard]] u64 submitted() const noexcept;
    [[nodiscard]] u64 executed() const noexcept;
    [[nodiscard]] u64 synchronous_calls() const noexcept;
    [[nodiscard]] u64 synchronous_calls_this_frame() const noexcept;
    /// Submissions refused because the queue or its arena was full.
    [[nodiscard]] u64 refused() const noexcept;

private:
    /// "no completion slot" — a fire-and-forget command, which is most of them.
    static constexpr u32 kNoCompletion = 0xFFFF'FFFFu;

    struct Record {
        CommandId id = 0;
        u32 argument_bytes = 0;
        /// Offset into the arena, or ~0u when the arguments are inline.
        u32 arena_offset = 0xFFFF'FFFFu;
        alignas(16) u8 inline_arguments[kInlineArgumentBytes] = {};
        void* result = nullptr;
        usize result_bytes = 0;
        /// Index into the queue's pool of completion events, or kNoCompletion.
        ///
        /// An index rather than a pointer to an event on the caller's stack, and that is a
        /// correctness matter rather than a style one: the owner thread may still be inside the
        /// notification when the waiter returns and its stack frame goes away. The queue owns the
        /// events, so they outlive every set() that can reach them.
        u32 completion = kNoCompletion;
    };

    struct Registration {
        CommandId id = 0;
        CommandHandler handler = nullptr;
        void* user = nullptr;
        const char* name = "";
        bool used = false;
    };

    Status enqueue(CommandId id, const void* arguments, usize argument_bytes, void* result,
                   usize result_bytes, u32 completion) noexcept;
    void execute(const Record& record) noexcept;
    [[nodiscard]] const Registration* find(CommandId id) const noexcept;
    /// Take a completion slot, or kNoCompletion when every one is in use.
    [[nodiscard]] u32 claim_completion() noexcept;
    void release_completion(u32 slot) noexcept;

    Config config_;
    Record* records_ = nullptr;
    Record* staging_ = nullptr;
    u8* arena_ = nullptr;
    Registration* registry_ = nullptr;
    Event* completions_ = nullptr;
    u32* completion_free_ = nullptr;

    mutable Mutex lock_;
    u32 count_ = 0;
    u32 completion_free_count_ = 0;
    usize arena_used_ = 0;

    Atomic<u64> submitted_{0};
    Atomic<u64> executed_{0};
    Atomic<u64> synchronous_{0};
    Atomic<u64> synchronous_this_frame_{0};
    Atomic<u64> refused_{0};
    Atomic<u64> owner_{0};
};

}  // namespace cy::jobs
