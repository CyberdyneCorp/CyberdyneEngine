#include <cy/servers/residency/deadline.h>

namespace cy::residency {

namespace {

[[nodiscard]] bool valid(PredictionSource source) noexcept {
    return static_cast<u32>(source) < static_cast<u32>(PredictionSource::Count);
}

}  // namespace

const char* prediction_source_name(PredictionSource source) noexcept {
    switch (source) {
        case PredictionSource::CameraMotion:
            return "camera-motion";
        case PredictionSource::WorldCell:
            return "world-cell";
        case PredictionSource::Sequence:
            return "sequence";
        case PredictionSource::Teleport:
            return "teleport";
        case PredictionSource::Network:
            return "network";
        case PredictionSource::Count:
            break;
    }
    return "unknown";
}

Status DeadlineBus::announce(const Prediction& prediction, f64 now) noexcept {
    if (!valid(prediction.source)) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "residency: prediction from an unknown source"});
    }

    const f64 due = now + ((prediction.seconds_until > 0.0) ? prediction.seconds_until : 0.0);

    for (u32 index = 0; index < kSubsystemCount; ++index) {
        const auto subsystem = static_cast<Subsystem>(index);
        if (!mask_contains(prediction.consumers, subsystem)) {
            continue;
        }

        const u64 key = slot_key(subsystem, prediction.region);
        if (usize* slot = slots_.find(key); slot != nullptr) {
            Deadline& existing = deadlines_[*slot];
            // EARLIER WINS. Two predictors disagreeing about how soon is the case this whole
            // mechanism exists to prevent, and when it happens anyway the urgent reading is the
            // safe one: preparing early costs memory, preparing late costs a missing frame.
            if (due < existing.due_seconds) {
                existing.due_seconds = due;
                existing.confidence = prediction.confidence;
                existing.source = prediction.source;
                existing.satisfied = false;
                existing.missed = false;
                ++accuracy_[static_cast<u32>(prediction.source)].deadlines_set;
            }
            continue;
        }

        Deadline deadline;
        deadline.subsystem = subsystem;
        deadline.region = prediction.region;
        deadline.due_seconds = due;
        deadline.confidence = prediction.confidence;
        deadline.source = prediction.source;
        if (Status pushed = deadlines_.push_back(deadline); !pushed) {
            return pushed;
        }
        if (auto placed = slots_.insert(key, deadlines_.size() - 1); !placed) {
            deadlines_.pop_back();
            return make_unexpected(placed.error());
        }
        ++accuracy_[static_cast<u32>(prediction.source)].deadlines_set;
    }
    return ok();
}

bool DeadlineBus::satisfy(Subsystem subsystem, u64 region, f64 now) noexcept {
    usize* slot = slots_.find(slot_key(subsystem, region));
    if (slot == nullptr) {
        return false;
    }
    Deadline& deadline = deadlines_[*slot];
    if (deadline.satisfied) {
        return true;
    }
    deadline.satisfied = true;
    PredictionAccuracy& counters = accuracy_[static_cast<u32>(deadline.source)];
    if (deadline.missed || now > deadline.due_seconds) {
        if (!deadline.missed) {
            deadline.missed = true;
            ++counters.deadlines_missed;
            ++misses_[static_cast<u32>(deadline.subsystem)];
        }
        return false;
    }
    ++counters.deadlines_met;
    return true;
}

u32 DeadlineBus::advance(f64 now) noexcept {
    u32 newly_missed = 0;
    for (Deadline& deadline : deadlines_) {
        if (deadline.satisfied || deadline.missed || now <= deadline.due_seconds) {
            continue;
        }
        deadline.missed = true;
        ++accuracy_[static_cast<u32>(deadline.source)].deadlines_missed;
        ++misses_[static_cast<u32>(deadline.subsystem)];
        ++newly_missed;
    }
    return newly_missed;
}

f64 DeadlineBus::seconds_until(Subsystem subsystem, u64 region, f64 now) const noexcept {
    const Deadline* deadline = find(subsystem, region);
    if (deadline == nullptr || deadline->satisfied) {
        return kNoDeadline;
    }
    const f64 remaining = deadline->due_seconds - now;
    return (remaining > 0.0) ? remaining : 0.0;
}

const Deadline* DeadlineBus::find(Subsystem subsystem, u64 region) const noexcept {
    const usize* slot = slots_.find(slot_key(subsystem, region));
    return (slot == nullptr) ? nullptr : &deadlines_[*slot];
}

u32 DeadlineBus::collect(Deadline* out, u32 capacity) const noexcept {
    if (out == nullptr) {
        return 0;
    }
    u32 written = 0;
    for (const Deadline& deadline : deadlines_) {
        if (written == capacity) {
            break;
        }
        out[written] = deadline;
        ++written;
    }
    return written;
}

u64 DeadlineBus::misses(Subsystem subsystem) const noexcept {
    const auto index = static_cast<u32>(subsystem);
    return (index < kSubsystemCount) ? misses_[index] : 0;
}

u32 DeadlineBus::outstanding(Subsystem subsystem) const noexcept {
    u32 count = 0;
    for (const Deadline& deadline : deadlines_) {
        if (deadline.subsystem == subsystem && !deadline.satisfied && !deadline.missed) {
            ++count;
        }
    }
    return count;
}

void DeadlineBus::record_prefetched(PredictionSource source, u64 pages) noexcept {
    if (valid(source)) {
        accuracy_[static_cast<u32>(source)].prefetched += pages;
    }
}

void DeadlineBus::record_sampled(PredictionSource source, u64 pages) noexcept {
    if (valid(source)) {
        accuracy_[static_cast<u32>(source)].sampled += pages;
    }
}

PredictionAccuracy DeadlineBus::accuracy(PredictionSource source) const noexcept {
    return valid(source) ? accuracy_[static_cast<u32>(source)] : PredictionAccuracy{};
}

PredictionAccuracy DeadlineBus::accuracy() const noexcept {
    PredictionAccuracy total;
    for (const PredictionAccuracy& entry : accuracy_) {
        total.prefetched += entry.prefetched;
        total.sampled += entry.sampled;
        total.deadlines_set += entry.deadlines_set;
        total.deadlines_met += entry.deadlines_met;
        total.deadlines_missed += entry.deadlines_missed;
    }
    return total;
}

void DeadlineBus::retire_resolved() noexcept {
    usize kept = 0;
    for (usize index = 0; index < deadlines_.size(); ++index) {
        const Deadline& deadline = deadlines_[index];
        if (deadline.satisfied || deadline.missed) {
            continue;
        }
        if (kept != index) {
            deadlines_[kept] = deadline;
        }
        ++kept;
    }
    while (deadlines_.size() > kept) {
        deadlines_.pop_back();
    }
    rebuild_slots();
}

void DeadlineBus::rebuild_slots() noexcept {
    slots_.clear();
    for (usize index = 0; index < deadlines_.size(); ++index) {
        const Deadline& deadline = deadlines_[index];
        if (auto placed = slots_.insert(slot_key(deadline.subsystem, deadline.region), index);
            !placed) {
            // Unreachable in practice: the table held these keys a moment ago and shrinking it
            // cannot need more room. Clearing rather than leaving a partial index, for the reason
            // `RequestQueue::compact` gives — a lookup structure that is half right is worse than
            // one that is empty, because the empty one is rebuilt on the next announcement.
            slots_.clear();
            return;
        }
    }
}

void DeadlineBus::clear() noexcept {
    deadlines_.clear();
    slots_.clear();
    for (PredictionAccuracy& entry : accuracy_) {
        entry = PredictionAccuracy{};
    }
    for (u64& entry : misses_) {
        entry = 0;
    }
}

}  // namespace cy::residency
