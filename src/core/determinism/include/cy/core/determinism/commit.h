#pragma once
// The commit boundary: the one point per tick at which state becomes authoritative. Task 4.2.3.
//
// `simulation-and-determinism` — "The commit boundary": each tick has a defined boundary reached
// after commands are ingested, systems execute, the task graph drains, per-worker structural
// buffers merge deterministically, events commit, and the state version increments. State is not
// authoritative before that point, and **every** consumer of authoritative state — hashing,
// rollback capture, replay checkpointing, save snapshotting, network send — keys off it rather than
// defining its own moment.
//
// --- WHY THIS IS A TYPE AND NOT A COMMENT --------------------------------------------------------
//
// "Everything keys off the same moment" is the kind of rule that is true on the day it is written
// and false two milestones later, because the second consumer needs its data slightly earlier and
// takes it there. The way to keep it true is for there to be nothing to take: a consumer does not
// *ask* when the tick committed, it is *called* with a `CommitRecord`, and the record is the only
// thing that says what tick's state it is looking at. A subsystem that wants to capture at its own
// moment has to add a call site, which is a change a reviewer sees.
//
// --- THE PHASES ----------------------------------------------------------------------------------
//
// "The tick pipeline SHALL define named phases so that ordering constraints and diagnostics share a
// vocabulary; the task scheduler SHALL still derive actual dependencies from declared access." The
// phases below are that vocabulary and nothing more — they are what a timing report is broken down
// by and what an ordering constraint names. They do not schedule anything: `ecs::Schedule` derives
// the graph from `jobs::AccessSet`s, and this enum has no way to reach it.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/memory/array.h>

namespace cy::determinism {

/// The named phases of a tick, in order. Shared vocabulary for constraints and diagnostics.
enum class TickPhase : u8 {
    /// Commands recorded before the tick are applied. Nothing has run yet.
    IngestCommands = 0,
    /// The four fixed-step system stages, whose internal order is `ecs::Stage`'s.
    RunSystems,
    /// The task graph drains: every job the systems submitted has completed.
    DrainTasks,
    /// Per-worker structural buffers merge in their deterministic order.
    MergeStructural,
    /// Events raised during the tick become visible.
    CommitEvents,
    /// The state version increments. Past this point the tick's state is authoritative.
    Commit,
};

inline constexpr u32 kTickPhaseCount = 6;

const char* tick_phase_name(TickPhase phase) noexcept;

/// What a tick committed. The only description of a moment that consumers of authoritative state
/// are given.
struct CommitRecord {
    SimulationPoint point;
    /// Increments exactly once per commit. Distinct from `ecs::World::version()`, which increments
    /// once per *stage* for change detection: this is the version of the tick, and it is what a
    /// snapshot, a replay record and a network packet stamp themselves with.
    u64 state_version = 0;
    /// Frame-scoped commands applied during `IngestCommands`, and structural changes merged during
    /// `MergeStructural`. Reported because "the cost of determinism is visible" begins with knowing
    /// how much there was of it.
    u32 commands_applied = 0;
    u64 structural_changes = 0;
    /// Nanoseconds from the start of `IngestCommands` to the end of `Commit`, measured by the
    /// runtime. A *diagnostic* — nothing authoritative reads it, which is why a duration may appear
    /// in a record whose whole point is that wall-clock time is not authoritative.
    u64 duration_ns = 0;
    /// The state hash, when one was taken at this boundary. `hashed` is false when the schedule did
    /// not call for one, and `hash` is then meaningless rather than zero-meaning-agreement.
    u64 hash = 0;
    bool hashed = false;
};

/// Something that reads authoritative state once a tick.
///
/// Hashing, rollback capture, replay checkpointing, save snapshotting and network send are all
/// this. The interface is deliberately narrow: an observer is told a tick committed and is given
/// the record; it is not given a hook before the commit, because the requirement is that nothing
/// observes a partial tick.
class CommitObserver {
public:
    CommitObserver() = default;
    virtual ~CommitObserver() = default;

    CommitObserver(const CommitObserver&) = delete;
    CommitObserver& operator=(const CommitObserver&) = delete;
    CommitObserver(CommitObserver&&) = delete;
    CommitObserver& operator=(CommitObserver&&) = delete;

    [[nodiscard]] virtual const char* name() const noexcept = 0;

    /// Called once, on the tick thread, after the state version has incremented. An error is
    /// reported and does not undo the commit: the tick happened, and a save that failed to write is
    /// a save failure rather than a simulation failure.
    [[nodiscard]] virtual Status on_commit(const CommitRecord& record) noexcept = 0;
};

/// The boundary itself: the counter, the observers, and the one call that crosses it.
class CommitBoundary {
public:
    explicit CommitBoundary(Allocator& allocator) noexcept : observers_(allocator) {}

    CommitBoundary(const CommitBoundary&) = delete;
    CommitBoundary& operator=(const CommitBoundary&) = delete;

    /// Refuses a duplicate name, and refuses to add an observer while a commit is in progress —
    /// an observer that registered from inside another's `on_commit` would see some ticks and not
    /// others depending on where it landed in the array.
    [[nodiscard]] Status observe(CommitObserver& observer) noexcept;

    /// Cross the boundary: increment the state version, stamp the record, and notify every
    /// observer in registration order.
    ///
    /// Registration order and not name order, deliberately, and it is the one place in this module
    /// where that is the right answer: an observer's *effects* are outside the simulation (a file
    /// written, a packet sent) and are not part of the state anything hashes, so ordering them by
    /// name would buy nothing and would make "the save runs before the network send" unexpressible.
    /// The state every observer reads is identical whatever order they run in, because the commit
    /// has already happened.
    ///
    /// The first observer error is returned after every observer has run: stopping at the first
    /// would silently skip the rest, and they are independent.
    [[nodiscard]] Expected<CommitRecord, Error> commit(const CommitRecord& tick) noexcept;

    [[nodiscard]] u64 state_version() const noexcept { return state_version_; }
    /// The last committed tick. What "a system queries authoritative state" observes: never a tick
    /// in progress, because this is only written by `commit()`.
    [[nodiscard]] const CommitRecord& last() const noexcept { return last_; }
    [[nodiscard]] u32 observer_count() const noexcept {
        return static_cast<u32>(observers_.size());
    }

    /// Resume a session recorded elsewhere. The state version is part of what a save carries, so a
    /// restored session continues the sequence rather than restarting it.
    void resume(u64 state_version, const CommitRecord& last) noexcept;

private:
    Array<CommitObserver*> observers_;
    CommitRecord last_;
    u64 state_version_ = 0;
    bool committing_ = false;
};

}  // namespace cy::determinism
