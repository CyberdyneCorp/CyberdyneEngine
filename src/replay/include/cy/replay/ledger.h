#pragma once
// The side-effect ledger: what must not be applied twice when a rollback re-simulates. M9 task 1.4.
//
// `replay-and-rollback` — "The side-effect ledger": presentation effects triggered by simulation
// "SHALL carry the **simulation point** that produced them, and the engine SHALL maintain a
// **ledger** of effects already realised. During re-simulation, an effect whose simulation point is
// already in the ledger SHALL be **suppressed**, so that rolling back and replaying a tick does not
// play the explosion twice."
//
// And "Speculative and confirmed effects": effects declare whether they may be realised on
// predicted state or only when the authority confirms them. "A muzzle flash may be speculative; an
// achievement, a currency change, or a persistent unlock SHALL NOT be."
//
// ================================================================================================
// THE KEY DOES NOT INCLUDE THE EPOCH, AND THAT IS A DECISION
// ================================================================================================
//
// `epoch.h` is explicit that a moment is an `(epoch, tick)` pair and that two points with the same
// tick in different epochs are **different moments**. A rollback restores a checkpoint, which
// advances the epoch (`EpochReason::CheckpointRestore`). So if the ledger keyed on the pair, the
// re-simulated tick would never match the entry the first simulation wrote, every explosion would
// play twice, and the ledger would be an elaborate way of storing nothing.
//
// **The ledger keys on `(kind, instance, tick)` and records the epoch beside it.** That is the
// whole content of what the ledger is for: the same tick, simulated again, is the *same effect*,
// and the fact that the timeline was reset in between is exactly what re-simulation means. The
// epoch is kept because a diagnostic wants to say "first realised in epoch 3, suppressed in epoch
// 4", and `first_epoch` is what the crash artefact reads.
//
// The `instance` half of the key is the caller's and it must be a function of simulation state
// rather than of a counter: two re-simulations of one tick must produce the same instance for the
// same effect, and a counter incremented per call does not. `realise()` cannot check that, so
// `tests/test_ledger.cpp` demonstrates the failure mode rather than the header merely warning about
// it.
//
// ================================================================================================
// BOUNDED, AND PRUNED BY THE WINDOW
// ================================================================================================
//
// "The ledger SHALL be bounded and pruned as the rollback window advances." Two bounds, because
// they fail differently: `prune_before()` is the ordinary one, driven by the rollback window's own
// advance, and `capacity` is the backstop for a session that never prunes — it drops the oldest
// entries and **counts what it dropped**, because a ledger that silently forgot would start
// permitting duplicates again and nothing would say so.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/memory/array.h>

namespace cy::replay {

/// May an effect be realised on predicted state?
enum class Speculation : u8 {
    /// Only once the authority has confirmed the state that produced it. An achievement, a currency
    /// change, a persistent unlock.
    ConfirmedOnly = 0,
    /// May play immediately and be reconciled if the prediction was wrong. A muzzle flash.
    Speculative,
};

/// What to do with an effect a re-simulation has invalidated. "Cancelled, allowed to finish, or
/// corrected, by declared policy per effect kind."
enum class Reconciliation : u8 {
    Cancel = 0,
    AllowToFinish,
    Correct,
};

const char* speculation_name(Speculation value) noexcept;
const char* reconciliation_name(Reconciliation value) noexcept;

/// One kind of effect, as its subsystem declares it.
struct EffectDeclaration {
    /// A stable identity. Never a pointer and never a registration index: the ledger outlives a
    /// rollback and a registration order does not.
    u64 kind = 0;
    /// A literal, for the diagnostic.
    const char* name = "";
    Speculation speculation = Speculation::ConfirmedOnly;
    Reconciliation reconciliation = Reconciliation::Cancel;
};

/// What the ledger decided about one attempt to realise an effect.
enum class EffectVerdict : u8 {
    /// Realise it. The first time this effect is seen at this tick.
    Realise = 0,
    /// Already in the ledger. **The explosion that does not play twice.**
    SuppressedAlreadyRealised,
    /// Declared `ConfirmedOnly` and offered on predicted state. Held, not dropped: `confirm()` lets
    /// it through when the authority catches up.
    DeferredUntilConfirmed,
    /// No declaration for this kind. Refused rather than assumed speculative — an undeclared
    /// achievement would otherwise fire on a prediction.
    Undeclared,
};

const char* effect_verdict_name(EffectVerdict verdict) noexcept;

/// One entry.
struct EffectEntry {
    u64 kind = 0;
    /// The caller's identity for this effect *within the tick*. A function of simulation state, not
    /// a counter — see the header.
    u64 instance = 0;
    u64 tick = 0;
    /// The epoch it was first realised in. Diagnostics: not part of the key.
    determinism::Epoch first_epoch;
    bool speculative = false;
    bool confirmed = false;
    bool invalidated = false;
};

/// What one session's ledger has done. Reported because a ledger that suppressed nothing and a
/// ledger that was never consulted read identically otherwise.
struct LedgerReport {
    u32 kinds_declared = 0;
    u32 realised = 0;
    u32 suppressed = 0;
    u32 deferred = 0;
    u32 undeclared = 0;
    u32 invalidated = 0;
    /// Entries dropped by the capacity backstop rather than by `prune_before()`. Non-zero means the
    /// ledger is no longer able to suppress that far back, and a session that sees it is a session
    /// whose window and whose ledger disagree.
    u32 dropped_for_capacity = 0;
};

/// The ledger.
class SideEffectLedger {
public:
    /// `capacity` is the backstop, in entries. Zero means the default.
    SideEffectLedger(Allocator& allocator, u32 capacity = kDefaultCapacity) noexcept
        : declarations_(allocator),
          entries_(allocator),
          capacity_(capacity == 0 ? kDefaultCapacity : capacity) {}

    SideEffectLedger(const SideEffectLedger&) = delete;
    SideEffectLedger& operator=(const SideEffectLedger&) = delete;

    static constexpr u32 kDefaultCapacity = 4096;

    /// Refuses a duplicate kind and a kind of zero, which is the null identity.
    [[nodiscard]] Status declare(const EffectDeclaration& declaration) noexcept;
    [[nodiscard]] const EffectDeclaration* find(u64 kind) const noexcept;

    /// **The gate.** Called by the system that would realise the effect, on every simulation of the
    /// tick — the first and every re-simulation.
    ///
    /// `predicted` is true when the state that produced it is a client-side prediction rather than
    /// confirmed authority.
    [[nodiscard]] EffectVerdict realise(u64 kind, u64 instance, determinism::SimulationPoint at,
                                        bool predicted) noexcept;

    /// The authority confirmed the tick. Deferred effects at or before it become realisable, and
    /// the caller is told how many so it can play them.
    [[nodiscard]] u32 confirm(u64 tick) noexcept;

    /// Re-simulation decided this effect should not have occurred. Returns the declared
    /// reconciliation for its kind so the caller acts on the declaration rather than on a guess.
    [[nodiscard]] Reconciliation invalidate(u64 kind, u64 instance, u64 tick) noexcept;

    /// Drop everything strictly before `tick`. Driven by the rollback window's advance.
    void prune_before(u64 tick) noexcept;

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(entries_.size()); }
    [[nodiscard]] u32 capacity() const noexcept { return capacity_; }
    [[nodiscard]] const EffectEntry& at(u32 index) const noexcept { return entries_[index]; }
    [[nodiscard]] const LedgerReport& report() const noexcept { return report_; }
    /// The oldest tick the ledger can still speak for. A caller asking about an earlier tick is
    /// asking about something the ledger has forgotten, and it should say so rather than answer.
    [[nodiscard]] u64 oldest_tick() const noexcept;

    void clear() noexcept;

private:
    [[nodiscard]] EffectEntry* find_entry(u64 kind, u64 instance, u64 tick) noexcept;
    void drop_oldest() noexcept;

    Array<EffectDeclaration> declarations_;
    Array<EffectEntry> entries_;
    LedgerReport report_;
    u32 capacity_ = kDefaultCapacity;
};

}  // namespace cy::replay
