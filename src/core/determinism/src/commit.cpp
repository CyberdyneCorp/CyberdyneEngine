#include <cy/core/determinism/commit.h>

#include <cstring>

namespace cy::determinism {

const char* tick_phase_name(TickPhase phase) noexcept {
    switch (phase) {
        case TickPhase::IngestCommands:
            return "ingest-commands";
        case TickPhase::RunSystems:
            return "run-systems";
        case TickPhase::DrainTasks:
            return "drain-tasks";
        case TickPhase::MergeStructural:
            return "merge-structural";
        case TickPhase::CommitEvents:
            return "commit-events";
        case TickPhase::Commit:
            return "commit";
    }
    return "unknown";
}

Status CommitBoundary::observe(CommitObserver& observer) noexcept {
    if (committing_) {
        return fail(ErrorCode::Unavailable,
                    "an observer cannot be added while a commit is in progress: it would see some "
                    "ticks and not others depending on where it landed in the array");
    }
    const char* name = observer.name();
    if (name == nullptr || *name == '\0') {
        return fail(ErrorCode::InvalidArgument, "a commit observer's name cannot be empty");
    }
    for (const CommitObserver* existing : observers_) {
        if (std::strcmp(existing->name(), name) == 0) {
            return fail(ErrorCode::AlreadyExists,
                        "a commit observer with this name is already registered");
        }
    }
    return observers_.push_back(&observer);
}

Expected<CommitRecord, Error> CommitBoundary::commit(const CommitRecord& tick) noexcept {
    CommitRecord record = tick;
    ++state_version_;
    record.state_version = state_version_;
    // Published before the observers run, so that an observer reading `last()` — a hasher asking
    // which tick it is hashing — sees this tick and not the previous one.
    last_ = record;

    committing_ = true;
    Status first_error = ok();
    for (CommitObserver* observer : observers_) {
        const Status notified = observer->on_commit(record);
        if (!notified && first_error) {
            first_error = notified;
        }
    }
    committing_ = false;

    if (!first_error) {
        return Unexpected<Error>(first_error.error());
    }
    return record;
}

void CommitBoundary::resume(u64 state_version, const CommitRecord& last) noexcept {
    state_version_ = state_version;
    last_ = last;
}

}  // namespace cy::determinism
