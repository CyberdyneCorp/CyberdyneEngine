#pragma once
// Simulation epochs, and the (epoch, tick) pair that names a moment. Task 4.2.2.
//
// `simulation-and-determinism` — "Simulation epochs": a moment in simulation is identified by an
// epoch and a tick, not by a tick alone, "because rollback moves the tick backwards". The epoch
// increments on disruptive resets — checkpoint restore, world reload, session restart, hot reload
// of gameplay code — and caches, handles, histories and logs carrying temporal assumptions compare
// epochs to detect that they are stale.
//
// WHY A PAIR AND NOT A MONOTONIC COUNTER. A monotonic "simulation sequence number" would make the
// two scenarios in the requirement trivially true and would throw away the thing that makes them
// useful: the tick is the authoritative unit of simulation time, so a log line, a replay record and
// a network packet all have to name it. The pair keeps the tick meaning what it means and adds a
// second component that says which run of it this is. Comparing the pair is then a rule everything
// can share, rather than a convention each subsystem invents.
//
// The epoch is NOT a version counter for authoritative state. That is the commit boundary's
// business (commit.h) and the two are deliberately separate: the state version increments once per
// tick, the epoch only when the timeline itself is disrupted.

#include <cy/core/base/types.h>

#include <compare>

namespace cy::determinism {

/// Why an epoch was left. Recorded so that a stale cache's diagnostic can say what invalidated it,
/// which is the difference between "your cache is stale" and "your cache is stale because a
/// checkpoint was restored at tick 41 200".
enum class EpochReason : u8 {
    /// The first epoch of a session. Never the reason for an *increment*.
    SessionStart = 0,
    CheckpointRestore,
    WorldReload,
    SessionRestart,
    HotReload,
};

/// The enumerator's own spelling, for a diagnostic. Never null.
const char* epoch_reason_name(EpochReason reason) noexcept;

/// Which run of the timeline this is. A counter, and deliberately not an index into anything.
struct Epoch {
    u32 value = 0;

    friend constexpr bool operator==(Epoch, Epoch) noexcept = default;
    friend constexpr auto operator<=>(Epoch, Epoch) noexcept = default;
};

/// A moment in simulation: which run of the timeline, and how far along it.
///
/// Ordering is lexicographic — epoch first — which is what makes "later" mean "later in this
/// session's history" rather than "further along some tick axis". Two points with the same tick and
/// different epochs are *different moments*, which is the whole content of the requirement's "the
/// same tick twice" scenario.
struct SimulationPoint {
    Epoch epoch;
    u64 tick = 0;

    friend constexpr bool operator==(const SimulationPoint&,
                                     const SimulationPoint&) noexcept = default;
    friend constexpr auto operator<=>(const SimulationPoint&,
                                      const SimulationPoint&) noexcept = default;
};

/// True when `stamp` was taken in a timeline that `now` no longer belongs to, or ahead of it.
///
/// Both halves matter and they fail differently. A different epoch means the timeline was reset
/// under the holder — the classic stale cache. The same epoch and a *later* tick means the holder
/// was written by a tick that has since been rolled back within this epoch, which a rollback that
/// does not bump the epoch (there is no such path today, and this is what would catch one) would
/// produce.
[[nodiscard]] constexpr bool is_stale(SimulationPoint stamp, SimulationPoint now) noexcept {
    return stamp.epoch != now.epoch || stamp.tick > now.tick;
}

/// The session's epoch, and why it last changed.
///
/// Held by the simulation clock (clock.h) rather than free, because a process may run more than one
/// world and a "current epoch" that is process-wide would make two independent timelines invalidate
/// each other's caches.
class EpochCounter {
public:
    EpochCounter() = default;

    [[nodiscard]] Epoch current() const noexcept { return epoch_; }
    [[nodiscard]] EpochReason reason() const noexcept { return reason_; }
    /// How many times the epoch has been left. Equal to `current().value` for a counter that
    /// started at zero, and reported separately so that a restored session which was given a
    /// starting epoch still says how much disruption *this process* saw.
    [[nodiscard]] u32 transitions() const noexcept { return transitions_; }

    /// Leave the current epoch. Returns the epoch entered.
    Epoch advance(EpochReason reason) noexcept {
        ++epoch_.value;
        ++transitions_;
        reason_ = reason;
        return epoch_;
    }

    /// Continue a session recorded elsewhere — a save, a replay header, a joining peer. The reason
    /// is required because an epoch adopted without one is indistinguishable in a log from an
    /// epoch that was never set.
    void adopt(Epoch epoch, EpochReason reason) noexcept {
        epoch_ = epoch;
        reason_ = reason;
    }

private:
    Epoch epoch_;
    EpochReason reason_ = EpochReason::SessionStart;
    u32 transitions_ = 0;
};

}  // namespace cy::determinism
