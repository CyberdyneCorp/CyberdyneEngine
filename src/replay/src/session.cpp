// The write side, and the divergence narrowed to a field. M9 section 3.

#include <cy/replay/session.h>

#include <cstdio>

namespace cy::replay {

// --- SessionRecorder -----------------------------------------------------------------------------

void SessionRecorder::attach(gameplay::CommandStream& stream) noexcept {
    stream.set_record_sink(gameplay::CommandStream::RecordSink{&SessionRecorder::on_command, this});
}

void SessionRecorder::detach(gameplay::CommandStream& stream) noexcept {
    stream.set_record_sink(gameplay::CommandStream::RecordSink{});
}

void SessionRecorder::on_command(void* user, const gameplay::Command& command) noexcept {
    auto* self = static_cast<SessionRecorder*>(user);
    LogRecord record;
    record.kind = RecordKind::Command;
    record.epoch = self->at_.epoch;
    // The command's own tick, not the recorder's point: `CommandStream::commit()` stamps the tick
    // it was committed for, and a recorder whose point had already moved on would file the command
    // under the wrong tick and make the replay produce it a tick late.
    record.tick = command.tick;
    // The stream's merge sequence, carried verbatim. `replay-and-rollback` asks for "a
    // per-participant sequence number within a tick, so that ordering is defined, duplicates are
    // detectable, and gaps are reportable", and re-numbering here would destroy all three.
    record.sequence = command.sequence;
    record.command = command;
    ++self->commands_;
    (void)self->emit(record);
}

Status SessionRecorder::emit(LogRecord& record) noexcept {
    if (Status appended = log_->append(record); !appended) {
        ++dropped_;
        return appended;
    }
    ++written_;
    if (ring_ != nullptr) {
        // The SAME record, not a second one built from the same inputs.
        ring_->push(record);
    }
    return ok();
}

Status SessionRecorder::record_state_hash(u64 hash) noexcept {
    LogRecord record;
    record.kind = RecordKind::StateHash;
    record.epoch = at_.epoch;
    record.tick = at_.tick;
    record.sequence = sequence_++;
    record.value = hash;
    return emit(record);
}

Status SessionRecorder::record_checkpoint(u64 address) noexcept {
    LogRecord record;
    record.kind = RecordKind::Checkpoint;
    record.epoch = at_.epoch;
    record.tick = at_.tick;
    record.sequence = sequence_++;
    record.value = address;
    return emit(record);
}

Status SessionRecorder::record_effect(u64 kind, u64 instance) noexcept {
    LogRecord record;
    record.kind = RecordKind::Effect;
    record.epoch = at_.epoch;
    record.tick = at_.tick;
    record.sequence = sequence_++;
    record.subject = kind;
    record.value = instance;
    return emit(record);
}

// --- DivergenceProbe -----------------------------------------------------------------------------

Status DivergenceProbe::observe(determinism::RunSide side, u64 tick, u64 root_hash) noexcept {
    return comparison_.observe(side, tick, root_hash);
}

Status DivergenceProbe::narrow(const determinism::StateHashTree& left,
                               const determinism::StateHashTree& right,
                               DivergenceReport& out) noexcept {
    out = DivergenceReport{};
    if (!comparison_.diverged()) {
        // THE REFUSAL. Two runs that agreed have no window and no field, and a function that
        // produced one anyway would be a report of something that did not happen.
        return fail(ErrorCode::InvalidArgument,
                    "replay: nothing to narrow — the two runs agreed at every compared tick");
    }
    determinism::localise(left, right, out.field);
    records_.clear();
    if (Status captured = cursor_.capture(comparison_, left, right, records_, out.window);
        !captured) {
        return captured;
    }
    out.valid = true;
    return ok();
}

void DivergenceProbe::clear() noexcept {
    comparison_.clear();
    records_.clear();
}

usize format_divergence(char* buffer, usize capacity, const DivergenceReport& report) noexcept {
    if (buffer == nullptr || capacity < kDivergenceReportBuffer) {
        return 0;
    }
    if (!report.valid || !report.field.diverged) {
        const int none = std::snprintf(buffer, capacity, "divergence: none");
        return none > 0 ? static_cast<usize>(none) : 0;
    }
    // THE SHAPE OF THE LINE IS THE CLAIM. Tick, entity, component, field, both values — and then
    // what it takes to reproduce it. A report that stopped after the tick would be the thing M9's
    // brief calls "worth much less".
    const int written = std::snprintf(
        buffer, capacity,
        "divergence: tick %llu entity %llu component %s(%llu) field %s(%llu) "
        "left=0x%016llx right=0x%016llx depth=%u%s | window: last-agreeing tick %llu, "
        "checkpoint %s, %u commands, %u external results, seed 0x%016llx",
        static_cast<unsigned long long>(report.window.first_diverging_tick),
        static_cast<unsigned long long>(report.field.entity), report.field.component_name,
        static_cast<unsigned long long>(report.field.component), report.field.field_name,
        static_cast<unsigned long long>(report.field.field),
        static_cast<unsigned long long>(report.field.left),
        static_cast<unsigned long long>(report.field.right), report.field.depth,
        report.field.shape_mismatch ? " SHAPE-MISMATCH" : "",
        static_cast<unsigned long long>(report.window.last_agreeing_tick),
        report.window.has_checkpoint ? "yes" : "none", report.window.command_count,
        report.window.external_count, static_cast<unsigned long long>(report.window.session_seed));
    return written > 0 ? static_cast<usize>(written) : 0;
}

}  // namespace cy::replay
