// Lockstep frames and resynchronisation. M9 task 3.3.

#include <cy/replay/lockstep.h>

#include <cy/core/determinism/hash.h>

namespace cy::replay {

const char* late_policy_name(LatePolicy policy) noexcept {
    switch (policy) {
        case LatePolicy::TreatAsNoCommand:
            return "TreatAsNoCommand";
        case LatePolicy::RepeatPrevious:
            return "RepeatPrevious";
        case LatePolicy::Predict:
            return "Predict";
        case LatePolicy::PauseSession:
            return "PauseSession";
        case LatePolicy::SubstituteAgent:
            return "SubstituteAgent";
        case LatePolicy::RemoveParticipant:
            return "RemoveParticipant";
    }
    return "Unknown";
}

const char* desync_policy_name(DesyncPolicy policy) noexcept {
    switch (policy) {
        case DesyncPolicy::Resynchronise:
            return "Resynchronise";
        case DesyncPolicy::Disconnect:
            return "Disconnect";
        case DesyncPolicy::Continue:
            return "Continue";
    }
    return "Unknown";
}

const char* desync_verdict_name(DesyncVerdict verdict) noexcept {
    switch (verdict) {
        case DesyncVerdict::Agreed:
            return "Agreed";
        case DesyncVerdict::Diverged:
            return "Diverged";
        case DesyncVerdict::NotCompared:
            return "NotCompared";
    }
    return "Unknown";
}

u64 late_decision_source(u64 participant) noexcept {
    // Mixed with the same function the state hash uses, so the identity is stable across runs and
    // cannot be confused with a bare participant handle that some other source happened to pick.
    return determinism::fold_hash(0x4C41'5445'4445'4144ULL /* "LATEDEAD" */, participant);
}

LockstepSession::LockstepSession(Allocator& allocator, RecordLog& log,
                                 const LockstepConfiguration& configuration) noexcept
    : participants_(allocator),
      delays_(allocator),
      own_hashes_(allocator),
      log_(&log),
      config_(configuration) {}

LockstepSession::Participant* LockstepSession::find(u64 participant) noexcept {
    for (Participant& entry : participants_) {
        if (entry.participant == participant && !entry.removed) {
            return &entry;
        }
    }
    return nullptr;
}

Status LockstepSession::add_participant(u64 participant) noexcept {
    if (participant == 0) {
        return fail(ErrorCode::InvalidArgument, "replay: a participant of zero is the null handle");
    }
    if (find(participant) != nullptr) {
        return fail(ErrorCode::AlreadyExists, "replay: that participant is already in the session");
    }
    return participants_.push_back(Participant{participant, Participant::kNever, 0, false});
}

Status LockstepSession::remove_participant(u64 participant) noexcept {
    Participant* found = find(participant);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "replay: no such participant");
    }
    // Marked rather than erased: the entry carries how many deadlines it missed, and a report that
    // lost that when the participant left would lose the explanation for why it left.
    found->removed = true;
    return ok();
}

u32 LockstepSession::participant_count() const noexcept {
    u32 present = 0;
    for (const Participant& entry : participants_) {
        if (!entry.removed) {
            ++present;
        }
    }
    return present;
}

u32 LockstepSession::input_delay(u64 tick) const noexcept {
    u32 delay = config_.input_delay_ticks;
    bool found = false;
    u64 latest = 0;
    // The change with the GREATEST effective tick at or before `tick`, and not the last one in
    // insertion order — two changes that arrived out of order must resolve the same way on every
    // peer, and a scan that took the last matching entry would resolve by arrival.
    for (const DelayChange& change : delays_) {
        if (change.effective_tick > tick) {
            continue;
        }
        if (!found || change.effective_tick > latest) {
            latest = change.effective_tick;
            delay = change.delay;
            found = true;
        }
    }
    return delay;
}

Status LockstepSession::schedule_input_delay(u32 delay, u64 effective_tick, u64 now_tick) noexcept {
    if (effective_tick <= now_tick) {
        return fail(
            ErrorCode::InvalidArgument,
            "replay: an input delay change takes effect in the future, never retroactively");
    }
    for (const DelayChange& change : delays_) {
        if (change.effective_tick == effective_tick) {
            return fail(ErrorCode::AlreadyExists,
                        "replay: a delay change is already scheduled for that tick");
        }
    }
    // Appended in arrival order; `input_delay()` resolves by tick rather than by position, so two
    // peers told about the same changes in different orders compute the same delay.
    return delays_.push_back(DelayChange{effective_tick, delay});
}

void LockstepSession::arrived(u64 participant, u64 tick) noexcept {
    Participant* found = find(participant);
    if (found == nullptr) {
        return;
    }
    // The newest arrival wins: a late packet for tick 40 that turns up after tick 41 arrived must
    // not make the session think 41 is missing.
    if (found->last_arrival == Participant::kNever || tick > found->last_arrival) {
        found->last_arrival = tick;
    }
}

Status LockstepSession::record_decision(u64 participant, LatePolicy policy, u64 tick,
                                        determinism::Epoch epoch) noexcept {
    LogRecord record;
    record.kind = RecordKind::ExternalResult;
    record.epoch = epoch;
    record.tick = tick;
    record.subject = late_decision_source(participant);
    record.value = static_cast<u64>(policy);
    // The participant travels in the payload as well as in the derived source id, so a person
    // reading the record does not have to invert a hash to learn who was late.
    record.payload_size = static_cast<u16>(sizeof(u64));
    for (u32 byte = 0; byte < 8; ++byte) {
        record.payload[byte] = static_cast<u8>((participant >> (byte * 8U)) & 0xFFU);
    }
    return log_->append(record);
}

Expected<LockstepFrame, Error> LockstepSession::close_frame(u64 tick,
                                                            determinism::Epoch epoch) noexcept {
    if (any_frame_ && tick <= last_frame_tick_) {
        return fail(ErrorCode::InvalidArgument, "replay: lockstep frames are closed in tick order");
    }

    LockstepFrame frame;
    frame.tick = tick;
    frame.epoch = epoch;
    frame.agreed_hash_tick = agreed_tick_;
    frame.agreed_hash = agreed_hash_;
    frame.has_agreement = has_agreement_;

    for (Participant& participant : participants_) {
        if (participant.removed) {
            continue;
        }
        ++frame.participants;
        const bool on_time =
            participant.last_arrival != Participant::kNever && participant.last_arrival >= tick;
        if (on_time) {
            ++frame.arrived;
            continue;
        }
        ++frame.late;
        ++participant.late_count;
    }

    if (frame.late == 0) {
        ++frames_closed_;
        last_frame_tick_ = tick;
        any_frame_ = true;
        return frame;
    }

    if (config_.late_policy == LatePolicy::PauseSession) {
        // The session does not advance. Nothing is recorded as consumed because nothing was, and
        // the frame is deliberately NOT counted as closed — a caller that retried would otherwise
        // see the frame count climb while the tick stood still.
        frame.paused = true;
        return frame;
    }

    for (Participant& participant : participants_) {
        if (participant.removed) {
            continue;
        }
        const bool on_time =
            participant.last_arrival != Participant::kNever && participant.last_arrival >= tick;
        if (on_time) {
            continue;
        }
        if (Status recorded =
                record_decision(participant.participant, config_.late_policy, tick, epoch);
            !recorded) {
            return make_unexpected(recorded.error());
        }
        ++late_decisions_;
        if (config_.late_policy == LatePolicy::RemoveParticipant) {
            participant.removed = true;
        }
    }

    ++frames_closed_;
    last_frame_tick_ = tick;
    any_frame_ = true;
    return frame;
}

bool LockstepSession::exchange_due(u64 tick) const noexcept {
    if (config_.hash_exchange_interval == 0) {
        return false;
    }
    return tick % config_.hash_exchange_interval == 0;
}

Status LockstepSession::publish_hash(u64 tick, u64 hash, determinism::Epoch epoch) noexcept {
    LogRecord record;
    record.kind = RecordKind::StateHash;
    record.epoch = epoch;
    record.tick = tick;
    record.value = hash;
    if (Status appended = log_->append(record); !appended) {
        return appended;
    }
    if (own_hashes_.size() >= kHashHistory) {
        // Drop the oldest by shifting: the history is 64 entries and a shift of 64 words once every
        // exchange is cheaper than the bookkeeping a ring would need to stay searchable by tick.
        for (usize index = 1; index < own_hashes_.size(); ++index) {
            own_hashes_[index - 1] = own_hashes_[index];
        }
        own_hashes_.pop_back();
    }
    return own_hashes_.push_back(PeerHash{tick, hash});
}

DesyncVerdict LockstepSession::observe_peer_hash(u64 participant, u64 tick, u64 hash) noexcept {
    for (const PeerHash& own : own_hashes_) {
        if (own.tick != tick) {
            continue;
        }
        if (own.hash == hash) {
            if (!has_agreement_ || tick > agreed_tick_) {
                agreed_tick_ = tick;
                agreed_hash_ = hash;
                has_agreement_ = true;
            }
            return DesyncVerdict::Agreed;
        }
        if (!diverged_) {
            diverged_ = true;
            first_diverging_ = tick;
            diverging_participant_ = participant;
        }
        return DesyncVerdict::Diverged;
    }
    // No local hash at that tick. Not an agreement and not a divergence: reporting either would be
    // an opinion about a comparison that did not happen.
    return DesyncVerdict::NotCompared;
}

bool LockstepSession::agreement(u64& tick, u64& hash) const noexcept {
    if (!has_agreement_) {
        return false;
    }
    tick = agreed_tick_;
    hash = agreed_hash_;
    return true;
}

Status LockstepSession::resynchronise(determinism::EpochCounter& epochs, u64 tick,
                                      u64 authoritative_hash, ResyncReport& out) noexcept {
    out = ResyncReport{};
    out.policy = config_.desync_policy;
    out.at_tick = tick;
    out.authoritative_hash = authoritative_hash;

    LogRecord record;
    record.kind = RecordKind::ExternalResult;
    record.tick = tick;
    record.subject = late_decision_source(0);
    record.value = authoritative_hash;
    record.sequence = static_cast<u32>(config_.desync_policy);

    if (config_.desync_policy == DesyncPolicy::Resynchronise) {
        // The world is about to be replaced by the authority's, which is a different thing from
        // restoring a capture this peer took — hence `WorldReload` rather than `CheckpointRestore`.
        out.epoch = epochs.advance(determinism::EpochReason::WorldReload);
        out.performed = true;
        record.epoch = out.epoch;
        // The agreement is now the authority's hash at this tick, by definition: this peer has
        // adopted it.
        agreed_tick_ = tick;
        agreed_hash_ = authoritative_hash;
        has_agreement_ = true;
        diverged_ = false;
        ++resyncs_;
    } else {
        record.epoch = epochs.current();
    }
    return log_->append(record);
}

}  // namespace cy::replay
