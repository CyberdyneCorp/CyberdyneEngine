#pragma once
// Four of the five readers of the one record. M9 task 1.5.
//
// The fifth — the divergence validator's window — is in divergence.h, because it needs the
// determinism validator's `FieldDivergence` and the other four do not.
//
// EVERY TYPE HERE DECLARES `using record_type = LogRecord;` AND NOTHING ELSE. That is what
// `ReadsTheOneRecord` constrains and what `bind_reader<>()` reads. A reader that grew a record of
// its own would not instantiate, and `log_readers()` — which is built out of `bind_reader<>()` —
// would carry a different record identity, which `tests/test_one_record.cpp` fails on by name.
//
// They are *cursors over a log they do not own*. None of them copies records, except the crash
// buffer, which has to: it is a ring that survives the log being gone, which is the whole of what a
// crash artefact is for.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/replay/log.h>
#include <cy/replay/record.h>

namespace cy::replay {

/// Reader 1: playback and seeking.
///
/// `replay-and-rollback`: "Replay playback SHALL be a **control source** producing recorded
/// commands, so playback exercises the same simulation path as live play." This cursor is what such
/// a control source reads; the control source itself is the runtime's, because layer 4 cannot name
/// a `ControlRegistry`'s owner.
class PlaybackCursor {
public:
    using record_type = LogRecord;

    explicit PlaybackCursor(const RecordLog& log) noexcept : log_(&log) {}

    /// Move to the first record at or after `tick`, through the log's index rather than by
    /// scanning. Returns the checkpoint the seek should restore first, if there is one at or before
    /// `tick`.
    [[nodiscard]] bool seek(u64 tick, LogRecord& checkpoint) noexcept;

    /// The next record at `tick`, or false when the tick has none left. Advances.
    [[nodiscard]] bool next_at(u64 tick, LogRecord& out) noexcept;

    /// The next command at `tick`. Skips hashes, checkpoints and effects — a control source
    /// produces commands, and the other kinds are for other readers.
    [[nodiscard]] bool next_command_at(u64 tick, LogRecord& out) noexcept;

    [[nodiscard]] u32 position() const noexcept { return position_; }
    [[nodiscard]] u32 records_read() const noexcept { return read_; }
    void rewind() noexcept;

private:
    const RecordLog* log_;
    u32 position_ = 0;
    u32 read_ = 0;
};

/// Reader 2: rollback re-simulation.
///
/// Deliberately a *half-open tick range* rather than a position: re-simulation is "restore the
/// checkpoint at `from`, then simulate ticks `from` through `to` from the recorded commands", and a
/// cursor that could be left half-way through a tick would re-simulate a tick with some of its
/// commands.
class RollbackCursor {
public:
    using record_type = LogRecord;

    explicit RollbackCursor(const RecordLog& log) noexcept : log_(&log) {}

    /// Prepare to re-simulate `[from_tick, to_tick]`. Refuses `to` before `from`.
    [[nodiscard]] Status open(u64 from_tick, u64 to_tick) noexcept;

    /// The commands for `tick`, appended to `out` in their recorded order. The order is the
    /// record's, never re-sorted: re-simulation "SHALL be identical in path to normal simulation".
    [[nodiscard]] Status commands_for(u64 tick, Array<LogRecord>& out) const noexcept;

    [[nodiscard]] u64 from_tick() const noexcept { return from_; }
    [[nodiscard]] u64 to_tick() const noexcept { return to_; }
    [[nodiscard]] bool open() const noexcept { return open_; }

private:
    const RecordLog* log_;
    u64 from_ = 0;
    u64 to_ = 0;
    bool open_ = false;
};

/// Reader 3: replication's input path.
///
/// **This cursor is here, in `src/replay/`, and not in `src/networking/`, on purpose.** It is the
/// seam `CY_NETWORKING` switches a driver on and off around: the *record* a peer sends is the
/// session's record whether or not the networking system is compiled in, and a replication layer
/// that declared its own would be the second representation `replay-and-rollback` forbids. With
/// `CY_NETWORKING=OFF` this cursor still exists and still compiles; what is gone is the transport
/// that would have drained it.
class ReplicationInputCursor {
public:
    using record_type = LogRecord;

    explicit ReplicationInputCursor(const RecordLog& log) noexcept : log_(&log) {}

    /// Every command for `tick` whose participant is `participant`, appended to `out`.
    /// A null participant means every participant, which is what a server relaying to spectators
    /// wants.
    [[nodiscard]] Status commands_for(u64 tick, u64 participant, Array<LogRecord>& out) noexcept;

    /// Commands handed out since construction. What a bandwidth budget is measured against.
    [[nodiscard]] u64 records_sent() const noexcept { return sent_; }

private:
    const RecordLog* log_;
    u64 sent_ = 0;
};

/// Reader 4: the crash replay buffer.
///
/// `replay-and-rollback` — "The crash replay buffer": a bounded ring of the most recent records,
/// flushed into the crash artefact.
///
/// **It owns its records and allocates once.** A ring written from a crash path cannot allocate,
/// and a ring holding pointers into a log would be a ring of dangling pointers precisely when it
/// matters. `reserve()` is called at session start and `push()` never allocates again, which is
/// what `tests/test_crash_buffer.cpp` asserts by exhausting the ring and checking the allocator's
/// count.
class CrashReplayBuffer {
public:
    using record_type = LogRecord;

    explicit CrashReplayBuffer(Allocator& allocator) noexcept : ring_(allocator) {}

    CrashReplayBuffer(const CrashReplayBuffer&) = delete;
    CrashReplayBuffer& operator=(const CrashReplayBuffer&) = delete;

    /// Fix the ring's size. Called once, before anything pushes.
    [[nodiscard]] Status reserve(u32 records) noexcept;

    /// Record one. Never allocates and never fails once reserved; overwrites the oldest.
    void push(const LogRecord& record) noexcept;

    /// Oldest first, into `out`. What the crash artefact carries.
    [[nodiscard]] Status flush(Array<LogRecord>& out) const noexcept;

    [[nodiscard]] u32 capacity() const noexcept { return static_cast<u32>(ring_.size()); }
    [[nodiscard]] u32 size() const noexcept { return filled_; }
    [[nodiscard]] u64 pushed() const noexcept { return pushed_; }
    /// Records the ring has overwritten. The window's own honesty: a crash artefact that covers the
    /// last two seconds of a ten-minute session should say so.
    [[nodiscard]] u64 overwritten() const noexcept {
        return pushed_ > filled_ ? pushed_ - filled_ : 0;
    }

    void clear() noexcept;

private:
    Array<LogRecord> ring_;
    u32 head_ = 0;
    u32 filled_ = 0;
    u64 pushed_ = 0;
};

}  // namespace cy::replay
