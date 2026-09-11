#pragma once
// The rollback loop. M9 task 3.2.
//
// Section 1 built `SnapshotRing` (the bounded window), `SideEffectLedger` (what must not run twice)
// and `RollbackCursor` (the commands for a tick). **This file is the loop that uses all three**,
// and it is the thing section 1's README lists as deliberately absent.
//
// ================================================================================================
// THE EXIT CRITERION IS A NEGATIVE, AND THE WHOLE FILE IS SHAPED BY IT
// ================================================================================================
//
// `docs/ROADMAP.md`: "Rollback re-simulates without re-applying ledgered side effects, **proven by
// a duplicate-effect test**." A negative claim is only ever proved by a control, so:
//
//   * **`offer()` is the only way a presentation effect gets realised.** If a system could call the
//     effect directly, the ledger would be advisory and the test would prove nothing about the
//     engine. One door, and the count of what came through it is `effects_offered()`.
//   * **`step` is handed no "am I re-simulating" flag.** `replay-and-rollback`: "Re-simulation
//   SHALL
//     be identical in path to normal simulation: the same systems, the same commit boundary, the
//     same commands." A flag would be an invitation to differ, and the first thing that differed
//     would be the thing the milestone exists to detect. The engine knows it is re-simulating; the
//     simulation does not.
//   * **The mutation is one line.** Replacing the body of `offer()` with an unconditional
//     `EffectVerdict::Realise` turns `tests/test_rollback.cpp`'s duplicate-effect case red. That
//     run is pasted in `src/replay/README.md`, because `replay-and-rollback`'s M9 delta requires
//     the demonstration rather than the assertion.
//
// ================================================================================================
// A CAPTURE AT (e, T) IS THE STATE *BEFORE* TICK T RAN
// ================================================================================================
//
// The convention has to be written down once and obeyed everywhere, because getting it wrong is a
// defect that looks exactly like a determinism bug: the re-simulation would skip or double a tick
// and the hash would differ for a reason that has nothing to do with floating point.
//
// A capture taken at `SimulationPoint{e, T}` is the world as it was when tick T began, before T's
// commands were applied. So `roll_back(T, U)` restores it and re-simulates T, T+1, ... U inclusive
// — which is exactly what `RollbackCursor::open(from, to)` is documented to serve, and why the loop
// below starts at the restored tick rather than after it.
//
// ================================================================================================
// A REFUSAL IS A REPORT, NOT AN ERROR
// ================================================================================================
//
// "a request older than the window SHALL be **reported** as requiring full resynchronisation rather
// than triggering an unbounded replay." So a rollback to a tick the ring has evicted returns `ok()`
// with `RollbackReport::refusal == WindowRefusal::RequiresResynchronisation` and `performed` false.
// A `Status` failure is reserved for a rollback that could not be *attempted* — no allocator, no
// hook, a world that refused a restore — because a caller that has to distinguish "too old" from
// "broken" by parsing a message will not.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/world.h>
#include <cy/replay/ledger.h>
#include <cy/replay/log.h>
#include <cy/replay/readers.h>
#include <cy/replay/record.h>
#include <cy/replay/snapshot.h>

namespace cy::replay {

/// The two things the session lends the rollback loop.
///
/// Function pointers and a user pointer rather than an interface, for `CommandStream::RecordSink`'s
/// reason: a virtual base declared here would be this module's type, and every session would
/// inherit from replay in order to be told to run a tick.
struct RollbackHooks {
    /// Hand `commands` to the session's producers, in recorded order. A session that drives its own
    /// command feed leaves this null and the loop skips it.
    ///
    /// The recommended implementation writes them into `gameplay::CommandStream` producers, exactly
    /// as `PlaybackDriver::produce()` does, so the re-simulated tick goes through the same merge,
    /// the same validation and the same commit boundary as the live one.
    Status (*feed)(void* user, u64 tick, Span<const LogRecord> commands) noexcept = nullptr;

    /// Run one tick. **The same function the live loop calls**, and it is handed no flag saying
    /// this is a re-simulation — see the header.
    Status (*step)(void* user, u64 tick) noexcept = nullptr;

    void* user = nullptr;

    [[nodiscard]] bool runnable() const noexcept { return step != nullptr; }
};

/// What one rollback did.
struct RollbackReport {
    /// `None` and `performed` true is the ordinary outcome. Anything else means nothing was
    /// re-simulated and the session must decide what to do instead.
    WindowRefusal refusal = WindowRefusal::None;
    bool performed = false;

    /// The tick the restored capture was taken at. Not necessarily the requested tick: the ring
    /// holds the captures it holds, and `find()` returns the one at or before the request.
    u64 restored_tick = 0;
    u64 through_tick = 0;
    /// The epoch the re-simulation ran in. A new one, because restoring a checkpoint leaves the old
    /// timeline — `EpochReason::CheckpointRestore`.
    determinism::Epoch epoch;

    u32 ticks_resimulated = 0;
    u32 commands_replayed = 0;
    /// Effects offered during this re-simulation, and how many the ledger stopped. **The
    /// duplicate-effect proof reads these two numbers**: a re-simulation that offered three
    /// explosions and suppressed three played none of them twice.
    u32 effects_offered = 0;
    u32 effects_suppressed = 0;
};

/// The loop.
class RollbackEngine {
public:
    RollbackEngine(Allocator& allocator, const RecordLog& log, SnapshotRing& ring,
                   SideEffectLedger& ledger, determinism::EpochCounter& epochs) noexcept
        : scratch_(allocator),
          log_(&log),
          ring_(&ring),
          ledger_(&ledger),
          epochs_(&epochs),
          at_{epochs.current(), 0} {}

    RollbackEngine(const RollbackEngine&) = delete;
    RollbackEngine& operator=(const RollbackEngine&) = delete;

    /// Where the simulation is. The live loop sets it once per tick; the rollback loop sets it for
    /// each tick it re-simulates. `offer()` reads it, which is what makes an effect carry "the
    /// simulation point that produced them" without every call site passing one.
    void set_point(determinism::SimulationPoint at) noexcept { at_ = at; }
    [[nodiscard]] determinism::SimulationPoint point() const noexcept { return at_; }

    /// **The one door a presentation effect comes through.**
    ///
    /// `instance` must be a function of simulation state — the entity, the projectile, the
    /// ability — and never a counter: two simulations of one tick must produce the same instance
    /// for the same effect, and a counter incremented per call does not. `tests/test_ledger.cpp`
    /// demonstrates that failure rather than warning about it.
    [[nodiscard]] EffectVerdict offer(u64 kind, u64 instance, bool predicted) noexcept;

    /// Restore the capture at or before `to_tick` and re-simulate through `through_tick`.
    ///
    /// Refuses (as a `Status`) a backwards range, absent hooks, or a world the capture will not
    /// restore into. **Reports** (as `RollbackReport::refusal`) a request the window cannot serve.
    [[nodiscard]] Status roll_back(u64 to_tick, u64 through_tick, ecs::World& world,
                                   const determinism::StateProviderRegistry& registry,
                                   const RollbackHooks& hooks, RollbackReport& out) noexcept;

    /// The window has advanced: nothing before `oldest_tick` can be rolled back to any more, so the
    /// ledger stops carrying entries for it. "The ledger SHALL be bounded and pruned as the
    /// rollback window advances", and this is the call that does the pruning.
    void advance_window(u64 oldest_tick) noexcept;

    [[nodiscard]] u32 rollbacks() const noexcept { return rollbacks_; }
    [[nodiscard]] u32 refusals() const noexcept { return refusals_; }
    [[nodiscard]] u64 effects_offered() const noexcept { return offered_; }
    [[nodiscard]] u64 effects_suppressed() const noexcept { return suppressed_; }
    [[nodiscard]] u64 effects_deferred() const noexcept { return deferred_; }

private:
    Array<LogRecord> scratch_;
    const RecordLog* log_;
    SnapshotRing* ring_;
    SideEffectLedger* ledger_;
    determinism::EpochCounter* epochs_;
    determinism::SimulationPoint at_;
    u32 rollbacks_ = 0;
    u32 refusals_ = 0;
    u64 offered_ = 0;
    u64 suppressed_ = 0;
    u64 deferred_ = 0;
    /// Counted per rollback so the report can carry this rollback's numbers rather than the
    /// session's running totals.
    u32 window_offered_ = 0;
    u32 window_suppressed_ = 0;
    bool resimulating_ = false;
};

}  // namespace cy::replay
