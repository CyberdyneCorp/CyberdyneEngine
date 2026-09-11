#pragma once
// Reader 5: the divergence window. M9 tasks 1.5 and 2.5's log half.
//
// `simulation-and-determinism` — "The determinism validator": "On divergence, the validator SHALL
// **capture the window**: the last agreeing snapshot, the commands in between, the random trace,
// the systems that wrote the differing state, and the divergent values."
//
// `cy::determinism::TickHashComparison` answers *which tick*, and `cy::determinism::localise()`
// answers *which field*. Neither can name a command, because both are layer 0. This is the half
// that can: it reads the one log between the last agreeing tick and the first diverging one and
// produces the small capture the requirement asks for — "a small capture from which the window can
// be replayed".
//
// WHAT IS IN THE WINDOW AND WHAT IS HONESTLY NOT:
//
//   the last agreeing snapshot   as the checkpoint record at or before the last agreeing tick,
//                                located through the log's index. The capture's *bytes* are the
//                                checkpoint store's; the window carries the address.
//   the commands in between      every `Command` record in the range, in order.
//   the random trace             **not recorded, and it does not need to be.** `random.h` makes a
//                                draw a pure function of `(seed, stream, point, entity, index)`
//                                with no cursor and no mutable state, so the trace is derivable
//                                from the seed in the manifest and the tick range. A recorded trace
//                                would be a second representation of something already determined,
//                                which is the failure mode this milestone is named after. The seed
//                                is carried instead.
//   the systems that wrote it    carried as the field's owning component and its subject id, which
//                                is what the engine knows. Attributing a write to a *system* needs
//                                a per-write attribution the ECS does not record, and inventing one
//                                here would be a second mechanism beside the firewall. Stated
//                                rather than quietly omitted.
//   the divergent values         both hashes at the deepest compared node, from `FieldDivergence`.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/validator.h>
#include <cy/core/memory/array.h>
#include <cy/replay/log.h>
#include <cy/replay/record.h>

namespace cy::replay {

/// The small capture. Everything needed to replay the window, and nothing that is derivable.
struct DivergenceWindow {
    bool valid = false;

    u64 last_agreeing_tick = 0;
    bool has_last_agreeing = false;
    u64 first_diverging_tick = 0;

    /// The checkpoint to restore first. `has_checkpoint` false means the window starts at the
    /// session's beginning, which is a fact a report should state rather than imply.
    LogRecord checkpoint;
    bool has_checkpoint = false;

    /// The session seed. What makes the random trace derivable rather than recorded.
    u64 session_seed = 0;

    /// The field, from `determinism::localise()`.
    determinism::FieldDivergence field;

    /// Commands in `(last_agreeing_tick, first_diverging_tick]`, in recorded order.
    u32 command_count = 0;
    /// External results in the same range. A divergence whose window contains one is a divergence
    /// worth suspecting the external result for first.
    u32 external_count = 0;
    /// The two state hashes recorded at the first diverging tick, when the log carries them.
    u64 left_hash = 0;
    u64 right_hash = 0;
};

/// Reader 5.
class DivergenceCursor {
public:
    using record_type = LogRecord;

    explicit DivergenceCursor(const RecordLog& log) noexcept : log_(&log) {}

    /// Assemble the window. `comparison` names the ticks; `left` and `right` are the two full hash
    /// trees taken at `comparison.first_diverging_tick()`, and `records` is appended with every
    /// record in the window, in order.
    ///
    /// Refuses a comparison that did not diverge: producing a "window" around no divergence is
    /// exactly the shape of a report that says something happened when nothing did.
    [[nodiscard]] Status capture(const determinism::TickHashComparison& comparison,
                                 const determinism::StateHashTree& left,
                                 const determinism::StateHashTree& right, Array<LogRecord>& records,
                                 DivergenceWindow& out) noexcept;

private:
    const RecordLog* log_;
};

}  // namespace cy::replay
