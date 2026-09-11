#pragma once
// Checkpoints: a save optimised for getting back into the session, not for leaving it.
//
// `save-and-persistence` — "Checkpoints and restore":
//
//   A **checkpoint** SHALL be a save optimised for rapid in-session restoration, retaining what is
//   required to restore session, world, and participant state without restarting the application.
//
//   Checkpoint restore SHALL increment the simulation epoch, so temporal caches and histories treat
//   themselves as stale.
//
//   Checkpoints MAY be retained in memory as well as on storage, subject to the memory budget.
//
// WHY THIS IS NOT `SaveService` WITH A DIFFERENT ENUMERATOR. `SaveKind::Checkpoint` already names a
// full generation written to storage, and that is the right thing for "the player quit and came
// back". It is the wrong thing for "the player died and the last checkpoint is restored", which is
// the scenario this requirement is written around: that path must not touch the filesystem at all
// when the state is still in memory, and it must leave the session running. So a checkpoint here is
// a RETAINED OVERLAY plus the moment it was taken at, and restoring one is a merge and an epoch
// increment rather than a load.
//
// THE EPOCH INCREMENT IS THE LOAD-BEARING PART, AND IT IS WHY THIS TYPE TAKES AN `EpochCounter`
// RATHER THAN RETURNING ONE. `simulation-and-determinism` makes a moment an (epoch, tick) pair
// precisely so that a restore can move the tick backwards without every cache, handle, history and
// log that carries a stamp silently believing itself current. A restore that did not advance the
// epoch would leave the tick going backwards inside one timeline, which `is_stale()` is written to
// catch and which nothing else would. Passing the counter in makes the increment impossible to
// forget: there is no overload of `restore()` that does not take one.
//
// THE BUDGET IS A REFUSAL, NOT AN AMBITION. "Subject to the memory budget" means a store that
// cannot hold another checkpoint evicts the oldest one rather than growing. An evicted checkpoint
// keeps its entry in the history — what it was and when — and loses its state, so a caller asking
// for one that is gone is told it is gone rather than handed something else.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/save/overlay.h>

namespace cy::save {

struct CheckpointConfig {
    /// How many checkpoints may be held in memory at once. The oldest is evicted to make room.
    u32 memory_slots = 2;
    /// The ceiling on what those slots may occupy, measured as the bytes the same state would take
    /// as chunks. Evicting to fit is what "subject to the memory budget" means.
    u64 memory_budget_bytes = 32ULL * 1024ULL * 1024ULL;
};

/// What one checkpoint is, whether or not its state is still held. An entry stays in the history
/// after eviction, because "there was a checkpoint at tick 41 200 and it is gone" is a different
/// answer from "there was never one".
struct CheckpointInfo {
    u32 id = 0;
    determinism::SimulationPoint point;
    /// The bytes the state occupies, measured the way the container would encode it.
    u64 bytes = 0;
    u32 regions = 0;
    u32 entries = 0;
    /// False once the budget or the slot count evicted its state.
    bool resident = false;
};

struct RestoreReport {
    u32 id = 0;
    /// The moment the checkpoint was taken at. The tick a caller sets its clock back to.
    determinism::SimulationPoint captured_at;
    /// The epoch before and after. Different, always: see the note at the top of this file.
    determinism::Epoch epoch_before;
    determinism::Epoch epoch_after;
    u32 regions = 0;
    u32 entries = 0;
};

/// A ring of retained checkpoints, newest last.
class CheckpointStore {
public:
    explicit CheckpointStore(Allocator& allocator = current_allocator()) noexcept
        : slots_(allocator), history_(allocator), allocator_(&allocator) {}

    CheckpointStore(const CheckpointStore&) = delete;
    CheckpointStore& operator=(const CheckpointStore&) = delete;

    [[nodiscard]] Status configure(const CheckpointConfig& config) noexcept;
    [[nodiscard]] const CheckpointConfig& config() const noexcept { return config_; }

    /// Take a checkpoint of `live` at `point`. Clones the overlay into a retained slot, evicting
    /// the oldest resident one when the slot count or the budget does not allow another.
    ///
    /// Fails rather than evicting when ONE checkpoint alone exceeds the budget: silently retaining
    /// nothing would make `restore()` fail later, at the worst moment, for a reason nothing said.
    [[nodiscard]] Expected<CheckpointInfo, Error> take(const Overlay& live,
                                                       determinism::SimulationPoint point) noexcept;

    /// Restore the newest resident checkpoint into `out`, replacing its contents, and ADVANCE the
    /// epoch with `EpochReason::CheckpointRestore`.
    [[nodiscard]] Expected<RestoreReport, Error> restore(Overlay& out,
                                                         determinism::EpochCounter& epoch) noexcept;

    /// Restore a named checkpoint. `NotFound` when it was never taken; `Unavailable` when it was
    /// taken and has since been evicted — two different answers, deliberately.
    [[nodiscard]] Expected<RestoreReport, Error> restore(u32 id, Overlay& out,
                                                         determinism::EpochCounter& epoch) noexcept;

    [[nodiscard]] u32 resident_count() const noexcept { return static_cast<u32>(slots_.size()); }
    [[nodiscard]] u64 resident_bytes() const noexcept;
    /// Every checkpoint ever taken by this store, oldest first, resident or not.
    [[nodiscard]] Span<const CheckpointInfo> history() const noexcept { return history_.span(); }
    [[nodiscard]] u32 evictions() const noexcept { return evictions_; }

    void clear() noexcept;

private:
    struct Slot {
        u32 id = 0;
        determinism::SimulationPoint point;
        u64 bytes = 0;
        Overlay state;

        explicit Slot(Allocator& allocator) noexcept : state(allocator) {}
    };

    [[nodiscard]] CheckpointInfo* find_info(u32 id) noexcept;
    void evict_oldest() noexcept;

    Array<Slot> slots_;
    Array<CheckpointInfo> history_;
    Allocator* allocator_ = nullptr;
    CheckpointConfig config_;
    u32 next_id_ = 1;
    u32 evictions_ = 0;
};

/// The bytes an overlay occupies, measured as the container would encode it — one chunk per region
/// plus one per scope with fragments. Exact rather than estimated, because a budget compared
/// against a guess is not a budget.
[[nodiscard]] Expected<u64, Error> measure_overlay_bytes(const Overlay& overlay) noexcept;

}  // namespace cy::save
