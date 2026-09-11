// External results: everything the simulation consumed that it did not compute. M9 task 1.2.

#include <cy/replay/external.h>

#include <cstring>

namespace cy::replay {

const char* external_kind_name(ExternalKind kind) noexcept {
    switch (kind) {
        case ExternalKind::ServiceResponse:
            return "ServiceResponse";
        case ExternalKind::MatchmakingAssignment:
            return "MatchmakingAssignment";
        case ExternalKind::Inference:
            return "Inference";
        case ExternalKind::SecureRandom:
            return "SecureRandom";
        case ExternalKind::WallClockDate:
            return "WallClockDate";
        case ExternalKind::Other:
            return "Other";
    }
    return "Other";
}

Status ExternalResults::declare(const ExternalSource& source) noexcept {
    if (source.id == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "replay: zero is the null external-source identity");
    }
    if (find(source.id) != nullptr) {
        return fail(ErrorCode::AlreadyExists, "replay: this external source is already declared");
    }
    if (Status added = sources_.push_back(source); !added) {
        return added;
    }
    ++report_.sources_declared;
    return ok();
}

const ExternalSource* ExternalResults::find(u64 id) const noexcept {
    for (const ExternalSource& source : sources_) {
        if (source.id == id) {
            return &source;
        }
    }
    return nullptr;
}

Status ExternalResults::adopt(const RecordLog& log) noexcept {
    records_.clear();
    for (const LogRecord& record : log.records()) {
        if (record.kind != RecordKind::ExternalResult) {
            continue;
        }
        Recorded recorded;
        recorded.source = record.subject;
        recorded.epoch = record.epoch;
        recorded.tick = record.tick;
        recorded.size = record.payload_size;
        std::memcpy(recorded.bytes, record.payload, kMaxRecordPayload);
        if (Status added = records_.push_back(recorded); !added) {
            return added;
        }
    }
    report_.recorded = static_cast<u32>(records_.size());
    return ok();
}

ExternalResults::Recorded* ExternalResults::match(u64 source,
                                                  determinism::SimulationPoint at) noexcept {
    // Matched on `(source, tick)` and not on the epoch, for the same reason the side-effect ledger
    // keys on the tick: a rollback advances the epoch, and re-simulating a tick must consume the
    // value the first simulation of that tick consumed. The epoch is kept for the diagnostic.
    for (Recorded& record : records_) {
        if (record.source == source && record.tick == at.tick && !record.consumed) {
            return &record;
        }
    }
    return nullptr;
}

Status ExternalResults::consume(u64 source, determinism::SimulationPoint at, ProduceFn produce,
                                void* user, RecordLog& log, Array<u8>& value) noexcept {
    const ExternalSource* declaration = find(source);
    if (declaration == nullptr) {
        // Refused in both modes. An undeclared source is a source a `ReplayStable` session did not
        // know it had to record, so accepting it here would be accepting a replay that cannot
        // reproduce.
        return fail(ErrorCode::NotFound,
                    "replay: an authoritative system consumed an external source it never "
                    "declared; declare it so the session records what it must");
    }
    ++report_.consumed;
    if (Status noted = consumed_.push_back(source); !noted) {
        return noted;
    }

    if (mode_ == ExternalMode::Replaying) {
        Recorded* recorded = match(source, at);
        if (recorded == nullptr) {
            // **The detection.** "Failing to record a consumed external source SHALL be
            // detectable: replay validation SHALL report an external consumption with no
            // corresponding record."
            ++report_.unrecorded_consumptions;
            if (report_.first_unrecorded[0] == '\0') {
                report_.first_unrecorded = declaration->name;
            }
            return fail(ErrorCode::NotFound,
                        "replay: this replay consumed an external result the recording does not "
                        "contain; the recording was incomplete, and replaying past it would "
                        "diverge silently");
        }
        recorded->consumed = true;
        value.clear();
        // `produce` is untouched. Not called and ignored, not called and compared: the requirement
        // is that the external source is not invoked, and `produce_invocations()` is how a test
        // checks a negative.
        (void)produce;
        (void)user;
        return value.append(Span<const u8>(recorded->bytes, recorded->size));
    }

    if (produce == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "replay: recording an external result needs the producer that makes it");
    }
    value.clear();
    ++produce_invocations_;
    if (Status produced = produce(user, value); !produced) {
        return produced;
    }
    if (value.size() > kMaxRecordPayload) {
        return fail(ErrorCode::BufferTooSmall,
                    "replay: an external result longer than a record's payload is content rather "
                    "than an outcome; record an asset identity instead");
    }

    LogRecord record;
    record.kind = RecordKind::ExternalResult;
    record.epoch = at.epoch;
    record.tick = at.tick;
    record.subject = source;
    record.value = static_cast<u64>(declaration->kind);
    record.payload_size = static_cast<u16>(value.size());
    std::memcpy(record.payload, value.data(), value.size());
    if (Status appended = log.append(record); !appended) {
        return appended;
    }
    ++report_.recorded;
    return ok();
}

const ExternalReport& ExternalResults::finish() noexcept {
    report_.unconsumed_records = 0;
    for (const Recorded& record : records_) {
        if (!record.consumed) {
            ++report_.unconsumed_records;
        }
    }
    return report_;
}

void ExternalResults::clear() noexcept {
    records_.clear();
    consumed_.clear();
    produce_invocations_ = 0;
    report_ = ExternalReport{};
    report_.sources_declared = static_cast<u32>(sources_.size());
}

}  // namespace cy::replay
