#pragma once
// The write side, and the divergence narrowed to a field. M9 tasks 3.1 and 3.2's other half.
//
// ================================================================================================
// TWO THINGS LIVE HERE AND THEY ARE THE TWO ENDS OF THE SAME CLAIM
// ================================================================================================
//
// `SessionRecorder` is what fills the log: it binds `CommandStream`'s seam, stamps the simulation
// point, and writes the four non-command record kinds a session produces. `DivergenceProbe` is what
// reads two runs of that log back and answers **which field of which component of which entity**
// first differed.
//
// M9's artefact makes three claims and the third is the hard one: not "these two runs diverge" but
// "this run diverged from that one at tick 37, on entity 5, in `Position.y`". A report that stops
// at the tick is a report that hands a person a haystack and says the needle is definitely in it.
//
// ================================================================================================
// THE RECORDER MIRRORS; IT DOES NOT FORK
// ================================================================================================
//
// The crash replay buffer is a *second destination for the same record*, not a second record. One
// `LogRecord` is built, appended to the log, and pushed into the ring; there is no path by which
// the two can disagree about what happened, because there is only one construction site.
// `replay-and-rollback`: "A second representation of participant intent SHALL NOT exist."
//
// ================================================================================================
// NARROWING IS THREE PIECES THAT ALREADY EXIST, AND THE GLUE IS THE POINT
// ================================================================================================
//
//   `determinism::TickHashComparison`   which tick     one u64 per tick per run
//   `determinism::localise()`           which field    two FULL trees at that one tick
//   `replay::DivergenceCursor`          the window     the log between the two ticks
//
// The split is what makes it affordable: a session hashes roots every tick and a full tree only at
// the tick the cheap half named. `DivergenceProbe` is the object that knows the order — and it
// **refuses to narrow a comparison that did not diverge**, because a "report" about no divergence
// is exactly the shape of a check that cannot fail.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/determinism/validator.h>
#include <cy/core/memory/array.h>
#include <cy/gameplay/command.h>
#include <cy/replay/divergence.h>
#include <cy/replay/log.h>
#include <cy/replay/readers.h>
#include <cy/replay/record.h>

namespace cy::replay {

/// The write side of a session: one place a `LogRecord` is built.
class SessionRecorder {
public:
    explicit SessionRecorder(RecordLog& log) noexcept : log_(&log) {}

    SessionRecorder(const SessionRecorder&) = delete;
    SessionRecorder& operator=(const SessionRecorder&) = delete;

    /// Bind `CommandStream`'s seam. One sink, and taking the stream by reference rather than
    /// storing it makes the lifetime the caller's, which it already is.
    void attach(gameplay::CommandStream& stream) noexcept;
    /// Unbind. Called before the recorder dies, and `tests/test_teardown.cpp`'s reason for
    /// existing: a sink left pointing at a dead recorder is a crash at the next commit.
    static void detach(gameplay::CommandStream& stream) noexcept;

    /// Also push every record into a bounded ring. Null clears it. The ring sees exactly what the
    /// log sees — see the header.
    void mirror_to(CrashReplayBuffer* ring) noexcept { ring_ = ring; }

    /// Where the simulation is. Stamped onto every record built from here until it changes.
    void set_point(determinism::SimulationPoint at) noexcept { at_ = at; }
    [[nodiscard]] determinism::SimulationPoint point() const noexcept { return at_; }

    [[nodiscard]] Status record_state_hash(u64 hash) noexcept;
    /// `address` is the checkpoint store's identifier for the capture. This module does not own the
    /// store; the record carries the address and nothing else.
    [[nodiscard]] Status record_checkpoint(u64 address) noexcept;
    [[nodiscard]] Status record_effect(u64 kind, u64 instance) noexcept;

    [[nodiscard]] u64 commands_recorded() const noexcept { return commands_; }
    [[nodiscard]] u64 records_written() const noexcept { return written_; }
    /// Records the log refused — a full allocator, a tick out of order. **Counted rather than
    /// swallowed**: a recorder that silently dropped records would produce a replay that stops
    /// reproducing its session at a point nothing reports.
    [[nodiscard]] u32 dropped() const noexcept { return dropped_; }

private:
    static void on_command(void* user, const gameplay::Command& command) noexcept;
    [[nodiscard]] Status emit(LogRecord& record) noexcept;

    RecordLog* log_;
    CrashReplayBuffer* ring_ = nullptr;
    determinism::SimulationPoint at_;
    u64 commands_ = 0;
    u64 written_ = 0;
    u32 sequence_ = 0;
    u32 dropped_ = 0;
};

/// A divergence, narrowed as far as the engine can narrow it, with the window that reproduces it.
struct DivergenceReport {
    bool valid = false;
    /// Which field, from `determinism::localise()`.
    determinism::FieldDivergence field;
    /// The last agreeing checkpoint, the commands in between, the seed. From `DivergenceCursor`.
    DivergenceWindow window;
};

/// The size of buffer `format_divergence()` needs. A constant rather than a guess, in
/// `format_arming_report()`'s shape and for its reason.
inline constexpr usize kDivergenceReportBuffer = 512;

/// One line a person reads, into a caller-provided buffer. Returns the bytes written, or zero.
///
/// The identifiers are printed **beside** the names, never instead of them: a component can be
/// renamed without the state changing, so the number is what the report is about and the name is
/// what makes it readable.
[[nodiscard]] usize format_divergence(char* buffer, usize capacity,
                                      const DivergenceReport& report) noexcept;

/// The order the three pieces go in, and the refusal that keeps it honest.
class DivergenceProbe {
public:
    DivergenceProbe(Allocator& allocator, const RecordLog& log) noexcept
        : comparison_(allocator), records_(allocator), cursor_(log) {}

    DivergenceProbe(const DivergenceProbe&) = delete;
    DivergenceProbe& operator=(const DivergenceProbe&) = delete;

    /// One tick's root hash from one run. Cheap enough to do every tick — one `u64` per side.
    [[nodiscard]] Status observe(determinism::RunSide side, u64 tick, u64 root_hash) noexcept;

    [[nodiscard]] bool diverged() const noexcept { return comparison_.diverged(); }
    [[nodiscard]] u64 first_diverging_tick() const noexcept {
        return comparison_.first_diverging_tick();
    }
    [[nodiscard]] u64 last_agreeing_tick() const noexcept {
        return comparison_.last_agreeing_tick();
    }
    [[nodiscard]] u32 ticks_compared() const noexcept { return comparison_.ticks_compared(); }
    [[nodiscard]] bool truncated() const noexcept { return comparison_.truncated(); }

    /// Narrow. `left` and `right` are the two **full** trees taken at `first_diverging_tick()`.
    ///
    /// **Refuses a comparison that did not diverge.** Producing a report about a divergence that
    /// did not happen is the exact shape of a check that cannot fail, and this project has shipped
    /// three of those.
    [[nodiscard]] Status narrow(const determinism::StateHashTree& left,
                                const determinism::StateHashTree& right,
                                DivergenceReport& out) noexcept;

    /// The records the window covers, filled by `narrow()`. What a reproduction artefact carries.
    [[nodiscard]] Span<const LogRecord> window_records() const noexcept { return records_.span(); }

    void clear() noexcept;

private:
    determinism::TickHashComparison comparison_;
    Array<LogRecord> records_;
    DivergenceCursor cursor_;
};

}  // namespace cy::replay
