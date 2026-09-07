#pragma once
// Capturing on the main thread, writing in the background. Task 6.2.
//
// `save-and-persistence` — "Consistent snapshots and background writing": a save is taken from a
// **tick-consistent** view of authoritative state captured at a commit boundary; serialisation,
// compression and writing proceed in the background while simulation continues; the main-thread
// cost of the capture is bounded and budgeted; and **the simulation is not paused for the duration
// of a save**.
//
// TWO THREADS AND ONE HANDOVER. `begin_save()` runs on the caller's thread — the main thread, at a
// commit boundary — and does one thing: it clones the overlay's records into a buffer this service
// owns. Everything after that (encoding, hashing, verification, the manifest, the switch) happens
// on the async service's thread, which is the only thread in this engine where blocking is legal:
// `core-jobs-and-concurrency` refuses a blocking region on a job worker, and file I/O is a blocking
// region. The engine's own asset system reaches the filesystem the same way and for the same
// reason.
//
// WHAT THE CAPTURE COSTS TODAY, STATED RATHER THAN IMPLIED. The specification requires the bounded
// main-thread cost to be achieved "by exploiting chunked storage — versioning or copy-on-write of
// the chunks that hold persistent state — rather than by copying the world". This implementation
// copies the OVERLAY, which is the delta and not the world: its cost is proportional to what has
// changed since the last compaction, not to the size of the world, and the exit criterion's
// unloaded region costs the same whether it is loaded or not. Copy-on-write over the ECS chunks is
// the next step, and it is a change to this file alone — the archive already takes an immutable
// snapshot and never reads the live overlay.
//
// WHY DIRTY FLAGS ARE NOT CLEARED HERE. A save that is captured, written and committed makes its
// captured records clean — but changes made DURING the write are not part of it, and clearing the
// live overlay when the write lands would silently drop them. Clearing exactly the records that
// were captured needs the per-chunk versioning above; until then, `poll()` reports what committed
// and the caller decides. The alternative — clearing everything on completion — is a save that
// loses a few seconds of play with no diagnostic, which is the failure this module exists to
// prevent.
//
// TEARDOWN IS A TESTED PATH, NOT AN AFTERTHOUGHT. `shutdown()` waits for an in-flight write and is
// idempotent, and the destructor calls it. M5.5's gate found this engine's first real defect in a
// subsystem torn down while a worker was still inside it, so src/save/tests/ destroys a service
// mid-write, repeatedly, rather than only after a quiet frame.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/jobs/async.h>
#include <cy/core/jobs/job_system.h>
#include <cy/save/archive.h>
#include <cy/save/overlay.h>

#include <atomic>

namespace cy::save {

/// What a requested save writes.
enum class SaveKind : u8 {
    /// A full generation: every region, a manifest, and the atomic switch.
    Checkpoint = 0,
    /// The dirty regions appended to the active generation's journal. Cheap and frequent.
    Journal = 1,
};

const char* save_kind_name(SaveKind kind) noexcept;

/// What one completed save did.
struct SaveOutcome {
    SaveKind kind = SaveKind::Checkpoint;
    /// The generation committed, or the one the journal extends.
    u32 generation = 0;
    bool succeeded = false;
    /// Set when it did not. `code` is `ErrorCode::None` on success.
    Error error;
    /// How many regions and entries the capture carried.
    u32 regions = 0;
    u32 entries = 0;
};

class SaveService {
public:
    explicit SaveService(Allocator& allocator = current_allocator()) noexcept
        : captured_(allocator) {}

    SaveService(const SaveService&) = delete;
    SaveService& operator=(const SaveService&) = delete;
    ~SaveService();

    /// Attach to the running job system, the async service and an open archive. None is owned.
    [[nodiscard]] Status start(jobs::JobSystem& jobs, jobs::AsyncService& async,
                               SaveArchive& archive, const SaveIdentity& identity) noexcept;

    /// Wait for an in-flight write and detach. Idempotent, and called by the destructor.
    void shutdown() noexcept;

    [[nodiscard]] bool is_running() const noexcept { return jobs_ != nullptr; }

    /// Capture `live` and start writing it. Fails with `Unavailable` while a save is in flight:
    /// two overlapping saves would race for the pointer, and refusing is what a game does with an
    /// autosave that arrives while a manual save is still writing.
    [[nodiscard]] Status begin_save(const Overlay& live, SaveKind kind) noexcept;

    [[nodiscard]] bool is_saving() const noexcept;

    /// Wait for the in-flight write. Runs other ready tasks rather than blocking a worker.
    void wait() noexcept;

    /// The last completed save, or nothing when none has completed since the last call.
    [[nodiscard]] Expected<SaveOutcome, Error> take_outcome() noexcept;

    [[nodiscard]] u64 saves_started() const noexcept { return started_; }
    [[nodiscard]] u64 saves_completed() const noexcept {
        return completed_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] u64 saves_failed() const noexcept {
        return failed_.load(std::memory_order_relaxed);
    }
    /// Nanoseconds the last capture cost on the calling thread. The budgeted number.
    [[nodiscard]] i64 last_capture_ns() const noexcept { return last_capture_ns_; }

private:
    static void write_operation(void* user) noexcept;
    void perform_write() noexcept;

    jobs::JobSystem* jobs_ = nullptr;
    jobs::AsyncService* async_ = nullptr;
    SaveArchive* archive_ = nullptr;
    SaveIdentity identity_;

    /// The immutable snapshot the background thread reads. Owned here, written only between
    /// `begin_save` and the operation's completion.
    Overlay captured_;
    SaveKind kind_ = SaveKind::Checkpoint;
    jobs::JobHandle in_flight_;
    /// Written by the writing thread, read by the caller's. `outcome_pending_` is the release/
    /// acquire pair that publishes it: a reader that sees the flag sees the outcome beside it.
    SaveOutcome outcome_;
    std::atomic<bool> outcome_pending_{false};

    u64 started_ = 0;
    std::atomic<u64> completed_{0};
    std::atomic<u64> failed_{0};
    i64 last_capture_ns_ = 0;
};

}  // namespace cy::save
