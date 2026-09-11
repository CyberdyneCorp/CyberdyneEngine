// Four of the five readers of the one record. M9 task 1.5.

#include <cy/replay/readers.h>

namespace cy::replay {

bool PlaybackCursor::seek(u64 tick, LogRecord& checkpoint) noexcept {
    position_ = log_->lower_bound(tick);
    return log_->nearest_checkpoint(tick, checkpoint);
}

void PlaybackCursor::rewind() noexcept {
    position_ = 0;
    read_ = 0;
}

bool PlaybackCursor::next_at(u64 tick, LogRecord& out) noexcept {
    if (position_ >= log_->size()) {
        return false;
    }
    const LogRecord& record = log_->at(position_);
    if (record.tick != tick) {
        return false;
    }
    out = record;
    ++position_;
    ++read_;
    return true;
}

bool PlaybackCursor::next_command_at(u64 tick, LogRecord& out) noexcept {
    while (position_ < log_->size()) {
        const LogRecord& record = log_->at(position_);
        if (record.tick != tick) {
            return false;
        }
        ++position_;
        if (record.kind != RecordKind::Command) {
            continue;
        }
        out = record;
        ++read_;
        return true;
    }
    return false;
}

Status RollbackCursor::open(u64 from_tick, u64 to_tick) noexcept {
    if (to_tick < from_tick) {
        return fail(ErrorCode::InvalidArgument,
                    "replay: a rollback window ends at or after it begins; backward simulation is "
                    "not attempted");
    }
    from_ = from_tick;
    to_ = to_tick;
    open_ = true;
    return ok();
}

Status RollbackCursor::commands_for(u64 tick, Array<LogRecord>& out) const noexcept {
    if (!open_) {
        return fail(ErrorCode::Unavailable, "replay: the rollback cursor has no window open");
    }
    if (tick < from_ || tick > to_) {
        return fail(ErrorCode::OutOfRange,
                    "replay: that tick is outside the window this cursor was opened for");
    }
    for (u32 index = log_->lower_bound(tick); index < log_->size(); ++index) {
        const LogRecord& record = log_->at(index);
        if (record.tick != tick) {
            break;
        }
        if (record.kind != RecordKind::Command) {
            continue;
        }
        if (Status added = out.push_back(record); !added) {
            return added;
        }
    }
    return ok();
}

Status ReplicationInputCursor::commands_for(u64 tick, u64 participant,
                                            Array<LogRecord>& out) noexcept {
    for (u32 index = log_->lower_bound(tick); index < log_->size(); ++index) {
        const LogRecord& record = log_->at(index);
        if (record.tick != tick) {
            break;
        }
        if (record.kind != RecordKind::Command) {
            continue;
        }
        if (participant != 0 && record.command.participant.bits() != participant) {
            continue;
        }
        if (Status added = out.push_back(record); !added) {
            return added;
        }
        ++sent_;
    }
    return ok();
}

Status CrashReplayBuffer::reserve(u32 records) noexcept {
    if (records == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "replay: a crash buffer of no records would report an empty window as a full "
                    "one");
    }
    ring_.clear();
    if (Status resized = ring_.resize(records); !resized) {
        return resized;
    }
    head_ = 0;
    filled_ = 0;
    pushed_ = 0;
    return ok();
}

void CrashReplayBuffer::push(const LogRecord& record) noexcept {
    if (ring_.empty()) {
        // Never reserved. Dropped rather than allocated: this is the path a crash handler takes and
        // it must not allocate. `pushed()` still counts, so the artefact can say it saw records it
        // could not keep.
        ++pushed_;
        return;
    }
    ring_[head_] = record;
    head_ = (head_ + 1) % static_cast<u32>(ring_.size());
    if (filled_ < ring_.size()) {
        ++filled_;
    }
    ++pushed_;
}

Status CrashReplayBuffer::flush(Array<LogRecord>& out) const noexcept {
    if (filled_ == 0) {
        return ok();
    }
    const u32 capacity = static_cast<u32>(ring_.size());
    const u32 first = (head_ + capacity - filled_) % capacity;
    for (u32 offset = 0; offset < filled_; ++offset) {
        if (Status added = out.push_back(ring_[(first + offset) % capacity]); !added) {
            return added;
        }
    }
    return ok();
}

void CrashReplayBuffer::clear() noexcept {
    head_ = 0;
    filled_ = 0;
    pushed_ = 0;
}

}  // namespace cy::replay
