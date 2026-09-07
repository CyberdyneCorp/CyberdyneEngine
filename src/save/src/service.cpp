// The save service: a bounded capture on the caller's thread, the write on the async thread.
// Task 6.2.

#include <cy/save/service.h>

#include <atomic>
#include <chrono>

namespace cy::save {
namespace {

i64 monotonic_nanoseconds() noexcept {
    // std::chrono directly: `platform::Platform` is layer 3 and this module is layer 2, so the
    // platform's clock is not reachable from here. The number is a budget report, not a simulation
    // input, so a steady clock read on the calling thread is exactly what it needs to be.
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

}  // namespace

const char* save_kind_name(SaveKind kind) noexcept {
    switch (kind) {
        case SaveKind::Checkpoint:
            return "checkpoint";
        case SaveKind::Journal:
            return "journal";
    }
    return "unknown";
}

SaveService::~SaveService() {
    shutdown();
}

Status SaveService::start(jobs::JobSystem& jobs, jobs::AsyncService& async, SaveArchive& archive,
                          const SaveIdentity& identity) noexcept {
    if (is_running()) {
        return fail(ErrorCode::AlreadyExists, "the save service is already running");
    }
    if (!archive.is_open()) {
        return fail(ErrorCode::Unavailable, "the save archive is not open");
    }
    if (!jobs.is_running()) {
        return fail(ErrorCode::Unavailable, "the job system is not running");
    }
    // The async service is NOT checked with `is_running()`. That predicate reports whether the
    // service THREAD has come up, which is a race against `start()` returning — a caller that did
    // everything right can still see false here, and this module lost a test run to exactly that
    // before reading what `AssetSystem::start` had already written down. The condition that matters
    // is that the service exists, and `submit_blocking` reports Unavailable when it does not, so a
    // service that was never started fails the first save with a message rather than being accepted
    // silently.
    jobs_ = &jobs;
    async_ = &async;
    archive_ = &archive;
    identity_ = identity;
    return ok();
}

void SaveService::shutdown() noexcept {
    // The order matters and is the whole of the teardown argument: WAIT first, then drop the
    // pointers the operation reads. Clearing them first would leave a worker inside
    // `perform_write()` holding an archive this object no longer admits to having.
    wait();
    jobs_ = nullptr;
    async_ = nullptr;
    archive_ = nullptr;
    in_flight_ = jobs::JobHandle();
}

bool SaveService::is_saving() const noexcept {
    if (jobs_ == nullptr || in_flight_.is_null()) {
        return false;
    }
    return !jobs_->is_complete(in_flight_);
}

void SaveService::wait() noexcept {
    if (jobs_ == nullptr || in_flight_.is_null()) {
        return;
    }
    jobs_->wait(in_flight_);
    in_flight_ = jobs::JobHandle();
}

Status SaveService::begin_save(const Overlay& live, SaveKind kind) noexcept {
    if (!is_running()) {
        return fail(ErrorCode::Unavailable, "the save service is not running");
    }
    if (is_saving()) {
        return fail(ErrorCode::Unavailable, "a save is already in flight");
    }
    // A completed handle is dropped here rather than in `is_saving()`, which is const and is asked
    // every frame.
    in_flight_ = jobs::JobHandle();

    const i64 began = monotonic_nanoseconds();
    if (Status cloned = live.clone_into(captured_); !cloned) {
        return cloned;
    }
    last_capture_ns_ = monotonic_nanoseconds() - began;

    kind_ = kind;
    outcome_ = SaveOutcome{};
    outcome_.kind = kind;
    outcome_.regions = static_cast<u32>(captured_.region_count());
    outcome_.entries = static_cast<u32>(captured_.entry_count());
    outcome_pending_.store(false, std::memory_order_relaxed);

    Expected<jobs::JobHandle, cy::Error> submitted =
        async_->submit_blocking(&SaveService::write_operation, this, "save.write");
    if (!submitted) {
        return make_unexpected(submitted.error());
    }
    in_flight_ = *submitted;
    ++started_;
    return ok();
}

void SaveService::write_operation(void* user) noexcept {
    static_cast<SaveService*>(user)->perform_write();
}

void SaveService::perform_write() noexcept {
    // Read once. `shutdown()` waits for this operation before it clears the pointer, so the archive
    // is alive for the whole of this call; the local copy is what makes that reasoning local.
    SaveArchive* archive = archive_;
    if (archive == nullptr) {
        outcome_.succeeded = false;
        outcome_.error = Error{ErrorCode::Unavailable, "the save archive went away", 0};
        failed_.fetch_add(1, std::memory_order_relaxed);
        outcome_pending_.store(true, std::memory_order_release);
        return;
    }

    if (kind_ == SaveKind::Journal) {
        const Status appended = archive->append_journal(captured_);
        outcome_.succeeded = appended.has_value();
        if (!appended) {
            outcome_.error = appended.error();
        } else {
            const Expected<u32, Error> active = archive->active_generation();
            outcome_.generation = active ? *active : 0U;
        }
    } else {
        const Expected<u32, Error> generation = archive->commit(captured_, identity_);
        outcome_.succeeded = generation.has_value();
        if (generation) {
            outcome_.generation = *generation;
        } else {
            outcome_.error = generation.error();
        }
    }

    if (outcome_.succeeded) {
        completed_.fetch_add(1, std::memory_order_relaxed);
    } else {
        failed_.fetch_add(1, std::memory_order_relaxed);
    }
    // Published last: a reader that sees the flag sees everything written before it.
    outcome_pending_.store(true, std::memory_order_release);
}

Expected<SaveOutcome, Error> SaveService::take_outcome() noexcept {
    if (!outcome_pending_.load(std::memory_order_acquire)) {
        return fail(ErrorCode::Unavailable, "no save has completed since the last call");
    }
    outcome_pending_.store(false, std::memory_order_relaxed);
    return outcome_;
}

}  // namespace cy::save
