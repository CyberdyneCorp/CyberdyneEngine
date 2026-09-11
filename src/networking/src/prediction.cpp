#include <cy/networking/prediction.h>

#include <algorithm>

namespace cy::net {

// --- InputBuffer ---------------------------------------------------------------------------------

Status InputBuffer::retain(const replay::LogRecord& record) noexcept {
    if (record.kind != replay::RecordKind::Command) {
        return fail(
            ErrorCode::InvalidArgument,
            "networking: an input buffer holds commands; a hash or an external result in it "
            "would be re-simulated as intent");
    }
    if (record.tick <= acknowledged_ && acknowledged_ != 0) {
        return fail(ErrorCode::OutOfRange,
                    "networking: that tick has already been acknowledged; retaining it would "
                    "replay an input the authority has already applied");
    }
    return records_.push_back(record);
}

u32 InputBuffer::acknowledge(u64 tick) noexcept {
    acknowledged_ = std::max(tick, acknowledged_);
    u32 dropped = 0;
    usize index = 0;
    while (index < records_.size()) {
        if (records_[index].tick > tick) {
            ++index;
            continue;
        }
        // Order-preserving removal: the buffer is replayed in recorded order, and
        // `replay-and-rollback` requires re-simulation to be "identical in path to normal
        // simulation", which a reordered input list would not be.
        for (usize slot = index + 1; slot < records_.size(); ++slot) {
            records_[slot - 1] = records_[slot];
        }
        records_.pop_back();
        ++dropped;
    }
    return dropped;
}

Status InputBuffer::replay_range(u64 from_tick, u64 to_tick,
                                 Array<replay::LogRecord>& out) const noexcept {
    if (to_tick < from_tick) {
        return fail(ErrorCode::InvalidArgument, "networking: a replay range ends before it begins");
    }
    for (const auto& record : records_) {
        if (record.tick < from_tick || record.tick > to_tick) {
            continue;
        }
        if (Status pushed = out.push_back(record); !pushed) {
            return pushed;
        }
    }
    return ok();
}

u64 InputBuffer::oldest_tick() const noexcept {
    return records_.empty() ? 0 : records_[0].tick;
}

u64 InputBuffer::newest_tick() const noexcept {
    return records_.empty() ? 0 : records_[records_.size() - 1].tick;
}

void InputBuffer::clear() noexcept {
    records_.clear();
    acknowledged_ = 0;
}

// --- PredictionLedger ----------------------------------------------------------------------------

const char* reconciliation_verdict_name(ReconciliationVerdict verdict) noexcept {
    switch (verdict) {
        case ReconciliationVerdict::Matched:
            return "Matched";
        case ReconciliationVerdict::Correct:
            return "Correct";
        case ReconciliationVerdict::Resynchronise:
            return "Resynchronise";
        case ReconciliationVerdict::NotPredicted:
            return "NotPredicted";
    }
    return "unknown";
}

Status PredictionLedger::predict(u64 tick, u64 state_hash) noexcept {
    newest_ = std::max(tick, newest_);
    for (auto& entry : predicted_) {
        if (entry.tick == tick) {
            entry.state_hash = state_hash;
            return ok();
        }
    }
    if (Status pushed = predicted_.push_back(PredictedTick{tick, state_hash}); !pushed) {
        return pushed;
    }
    // Evict what has fallen outside the window. The eviction is here rather than in a separate call
    // because a window that is only trimmed when someone remembers is not a bound.
    const u64 oldest = newest_ > policy_.window_ticks ? newest_ - policy_.window_ticks : 0;
    usize index = 0;
    while (index < predicted_.size()) {
        if (predicted_[index].tick + 1 <= oldest) {
            predicted_.remove_unordered(index);
            continue;
        }
        ++index;
    }
    return ok();
}

ReconciliationVerdict PredictionLedger::compare(u64 tick, u64 authoritative_hash,
                                                u64 magnitude) noexcept {
    ++report_.compared;
    const u64 oldest = newest_ > policy_.window_ticks ? newest_ - policy_.window_ticks : 0;
    if (newest_ != 0 && tick < oldest) {
        ++report_.resynchronised;
        return ReconciliationVerdict::Resynchronise;
    }
    for (auto& entry : predicted_) {
        if (entry.tick != tick) {
            continue;
        }
        if (entry.state_hash == authoritative_hash || magnitude <= policy_.tolerance) {
            ++report_.matched;
            return ReconciliationVerdict::Matched;
        }
        ++report_.corrected;
        report_.last_corrected_tick = tick;
        report_.largest_error = std::max(magnitude, report_.largest_error);
        return ReconciliationVerdict::Correct;
    }
    ++report_.not_predicted;
    return ReconciliationVerdict::NotPredicted;
}

void PredictionLedger::clear() noexcept {
    predicted_.clear();
    report_ = ReconciliationReport{};
    newest_ = 0;
}

// --- CorrectionSmoother --------------------------------------------------------------------------

void CorrectionSmoother::begin(u64 tick, u32 smoothing_ticks) noexcept {
    began_ = tick;
    ticks_ = smoothing_ticks;
    running_ = smoothing_ticks != 0;
    ++corrections_;
}

bool CorrectionSmoother::active(u64 tick) const noexcept {
    return running_ && tick >= began_ && (tick - began_) < ticks_;
}

u32 CorrectionSmoother::weight_at(u64 tick) const noexcept {
    if (!running_ || ticks_ == 0 || tick <= began_) {
        return running_ && ticks_ != 0 ? 0U : 100U;
    }
    const u64 elapsed = tick - began_;
    if (elapsed >= ticks_) {
        return 100;
    }
    return static_cast<u32>((elapsed * 100ULL) / ticks_);
}

// --- ProxyHistory --------------------------------------------------------------------------------

const char* rewind_refusal_name(RewindRefusal refusal) noexcept {
    switch (refusal) {
        case RewindRefusal::None:
            return "None";
        case RewindRefusal::OutsideWindow:
            return "OutsideWindow";
        case RewindRefusal::ImplausibleForLatency:
            return "ImplausibleForLatency";
        case RewindRefusal::InTheFuture:
            return "InTheFuture";
    }
    return "unknown";
}

ProxyHistory::ProxyHistory(Allocator& allocator, u32 window_ticks) noexcept
    : slots_(allocator), proxies_(allocator), window_(window_ticks == 0 ? 1 : window_ticks) {}

Status ProxyHistory::record(u64 tick, Span<const CollisionProxy> proxies) noexcept {
    if (slots_.empty()) {
        if (Status sized = slots_.resize(window_); !sized) {
            return sized;
        }
        // One contiguous block, sized for the worst case the caller has shown us so far. The ring
        // grows when a tick carries more proxies than any before it and never after that, which is
        // what keeps a per-tick allocation out of the server's frame.
    }
    const u32 slot = static_cast<u32>(tick % window_);
    const u32 stride = static_cast<u32>(proxies.size());
    const u32 first = slot * stride;
    if (stride != 0 && proxies_.size() < static_cast<usize>(window_) * stride) {
        if (Status sized = proxies_.resize(static_cast<usize>(window_) * stride); !sized) {
            return sized;
        }
    }
    for (u32 index = 0; index < stride; ++index) {
        proxies_[first + index] = proxies[index];
    }
    slots_[slot] = Slot{tick, first, stride, true};
    ++recorded_;
    return ok();
}

RewindRefusal ProxyHistory::rewind(u64 tick, u64 now_tick, u32 peer_latency_ticks,
                                   Array<CollisionProxy>& out) const noexcept {
    if (tick > now_tick) {
        ++refusals_;
        return RewindRefusal::InTheFuture;
    }
    if (now_tick - tick >= window_) {
        ++refusals_;
        return RewindRefusal::OutsideWindow;
    }
    // The latency check, and the reason it is a separate refusal: a claim inside the window is
    // still a claim this particular peer could not honestly make. One tick of slack covers the
    // rounding between a measured round trip and a tick count.
    if (now_tick - tick > static_cast<u64>(peer_latency_ticks) + 1) {
        ++refusals_;
        return RewindRefusal::ImplausibleForLatency;
    }
    if (slots_.empty()) {
        ++refusals_;
        return RewindRefusal::OutsideWindow;
    }
    const u32 slot = static_cast<u32>(tick % window_);
    if (!slots_[slot].filled || slots_[slot].tick != tick) {
        ++refusals_;
        return RewindRefusal::OutsideWindow;
    }
    for (u32 index = 0; index < slots_[slot].count; ++index) {
        if (!out.push_back(proxies_[slots_[slot].first + index])) {
            return RewindRefusal::OutsideWindow;
        }
    }
    return RewindRefusal::None;
}

}  // namespace cy::net
