// Reader 5: the divergence window. M9 tasks 1.5 and 2.5's log half.

#include <cy/replay/divergence.h>

namespace cy::replay {

Status DivergenceCursor::capture(const determinism::TickHashComparison& comparison,
                                 const determinism::StateHashTree& left,
                                 const determinism::StateHashTree& right, Array<LogRecord>& records,
                                 DivergenceWindow& out) noexcept {
    out = DivergenceWindow{};
    if (!comparison.diverged()) {
        // Producing a "window" around no divergence is exactly the shape of a report that says
        // something happened when nothing did, which is the defect class this milestone's own
        // criteria are held to.
        return fail(ErrorCode::InvalidArgument, "replay: no divergence to capture a window around");
    }

    out.first_diverging_tick = comparison.first_diverging_tick();
    const u64 last_agreeing = comparison.last_agreeing_tick();
    out.has_last_agreeing = last_agreeing != determinism::kNoTick;
    out.last_agreeing_tick = out.has_last_agreeing ? last_agreeing : 0;
    out.session_seed = log_->manifest().session_seed;

    determinism::localise(left, right, out.field);

    LogRecord checkpoint;
    if (log_->nearest_checkpoint(out.has_last_agreeing ? out.last_agreeing_tick : 0, checkpoint)) {
        out.checkpoint = checkpoint;
        out.has_checkpoint = true;
    }

    // The window is (last agreeing, first diverging]. Half-open at the bottom because the last
    // agreeing tick's own commands have already been accounted for by the state that agreed.
    const u64 from = out.has_last_agreeing ? out.last_agreeing_tick + 1 : 0;
    for (u32 index = log_->lower_bound(from); index < log_->size(); ++index) {
        const LogRecord& record = log_->at(index);
        if (record.tick > out.first_diverging_tick) {
            break;
        }
        if (Status added = records.push_back(record); !added) {
            return added;
        }
        switch (record.kind) {
            case RecordKind::Command:
                ++out.command_count;
                break;
            case RecordKind::ExternalResult:
                ++out.external_count;
                break;
            case RecordKind::StateHash:
                if (record.tick == out.first_diverging_tick) {
                    out.left_hash = record.value;
                }
                break;
            case RecordKind::Checkpoint:
            case RecordKind::Effect:
                break;
        }
    }
    out.right_hash = right.root_hash();
    if (out.left_hash == 0) {
        out.left_hash = left.root_hash();
    }
    out.valid = true;
    return ok();
}

}  // namespace cy::replay
