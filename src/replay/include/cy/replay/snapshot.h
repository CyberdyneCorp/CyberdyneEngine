#pragma once
// Snapshot kinds, checkpoints, and the bounded rollback window. M9 task 1.3.
//
// `replay-and-rollback` — "Snapshot kinds": four kinds that "share schema and state classification
// and **deliberately do not share encoding**", and "**The save encoding SHALL NOT be used for
// rollback.** A format that tolerates schema evolution cannot be fast enough to capture at
// simulation rate, and a format fast enough cannot tolerate schema evolution."
//
// ================================================================================================
// GENERALISING `cy::ecs::Snapshot` RATHER THAN REPLACING IT
// ================================================================================================
//
// `cy::ecs::Snapshot` already is the fast current-layout encoding: it copies the entity table as
// records and each archetype's columns as bytes, deep-copies buffer spills, and **restores entity
// identifiers exactly**. That last property is not a detail. `simulation-and-determinism` folds
// entity identity into the state hash, so "a snapshot restores identity, not merely value" is the
// difference between a checkpoint that reproduces a hash and one that reports a divergence on every
// entity it restored.
//
// So this file does not write a second snapshot. It adds the two things `ecs::Snapshot` cannot
// know about — the **kind**, which decides the encoding and which providers take part, and the
// **provider state** beside the entity state, which `replay-and-rollback` requires ("random streams
// and provider state SHALL be restored with entity state").
//
// ================================================================================================
// WHAT IS HERE, AND THE ONE GAP, STATED RATHER THAN DISCOVERED
// ================================================================================================
//
//   Rollback          `ecs::Snapshot` in memory, plus provider bytes. Restores identity exactly.
//   ReplayCheckpoint  the same, plus the provider bytes run through log.h's deterministic
//                     run-length encoding. Restores identity exactly.
//   SaveCheckpoint    `ecs::serialize`'s tagged, versioned, self-describing stream. **Capture
//                     only**: `restore()` refuses it, because `ecs::deserialize` *appends* fresh
//                     entities rather than restoring identifiers, so restoring one would produce a
//                     world with the right values under the wrong identities — a divergence by the
//                     hash's own definition. Loading a save is `save-and-persistence`'s path into a
//                     fresh world, which is the case it is correct for.
//   DebugCapture      the same stream, uncompressed, plus derived metadata.
//
// **THE GAP.** A *file-backed* replay checkpoint that restores identity verbatim needs an
// identity-preserving serialiser for `ecs::Snapshot`, and `src/ecs/` has none: `serialize()` writes
// values and `deserialize()` mints new entities. A `ReplayCheckpoint` here is therefore an
// in-memory capture with compressible provider state, which is what a seek within a running session
// needs and is not what writing a two-hour replay to disk needs. Saying so now is cheaper than
// discovering it at the gate.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/determinism/provider.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/snapshot.h>
#include <cy/ecs/world.h>

namespace cy::replay {

/// The four kinds, and they differ in encoding on purpose.
enum class SnapshotKind : u8 {
    /// Restore latency, captured many times per second.
    Rollback = 0,
    /// Compact restore for seeking.
    ReplayCheckpoint,
    /// Surviving code and schema change.
    SaveCheckpoint,
    /// Diagnosis. May carry derived state and metadata.
    DebugCapture,
};

/// How a kind stores what it captured. Two kinds sharing an encoding would be two names for one
/// thing; these four do not.
enum class SnapshotEncoding : u8 {
    /// Current layout, bulk copy, in memory. `ecs::Snapshot`.
    CurrentLayoutInMemory = 0,
    /// Current layout, with the provider half compressed.
    CurrentLayoutCompressed,
    /// Tagged, versioned, migratable. `ecs::serialize`.
    TaggedVersioned,
    /// Tagged and versioned, plus metadata a diagnosis wants and a restore does not.
    Rich,
};

const char* snapshot_kind_name(SnapshotKind kind) noexcept;
const char* snapshot_encoding_name(SnapshotEncoding encoding) noexcept;

[[nodiscard]] constexpr SnapshotEncoding encoding_of(SnapshotKind kind) noexcept {
    switch (kind) {
        case SnapshotKind::Rollback:
            return SnapshotEncoding::CurrentLayoutInMemory;
        case SnapshotKind::ReplayCheckpoint:
            return SnapshotEncoding::CurrentLayoutCompressed;
        case SnapshotKind::SaveCheckpoint:
            return SnapshotEncoding::TaggedVersioned;
        case SnapshotKind::DebugCapture:
            return SnapshotEncoding::Rich;
    }
    return SnapshotEncoding::CurrentLayoutInMemory;
}

/// Which providers take part in a kind. `provider.h`'s declaration, read per kind rather than each
/// kind keeping its own list — "Participation SHALL be explicit rather than assumed."
[[nodiscard]] constexpr determinism::Participates participation_for(SnapshotKind kind) noexcept {
    switch (kind) {
        case SnapshotKind::Rollback:
            return determinism::Participates::Rollback;
        case SnapshotKind::ReplayCheckpoint:
            return determinism::Participates::Checkpoint;
        case SnapshotKind::SaveCheckpoint:
            return determinism::Participates::Save;
        case SnapshotKind::DebugCapture:
            // Everything a provider will give, because a diagnosis that omitted the expensive half
            // is a diagnosis of the cheap half.
            return determinism::Participates::Hash | determinism::Participates::Rollback |
                   determinism::Participates::Checkpoint | determinism::Participates::Save;
    }
    return determinism::Participates::None;
}

/// One capture of one kind: entity state, provider state, and the moment.
class StateCapture {
public:
    StateCapture(Allocator& allocator, SnapshotKind kind) noexcept
        : entities_(allocator), stream_(allocator), providers_(allocator), kind_(kind) {}

    StateCapture(const StateCapture&) = delete;
    StateCapture& operator=(const StateCapture&) = delete;

    /// Capture `world` and every provider that declared this kind's participation.
    ///
    /// The registry must be finalised: an unfinalised one has an order that depends on when plugins
    /// loaded, and capturing in that order would make load order part of the capture.
    [[nodiscard]] Status capture(ecs::World& world,
                                 const determinism::StateProviderRegistry& registry,
                                 determinism::SimulationPoint at) noexcept;

    /// Put it back. Refuses `SaveCheckpoint` — see the header: `ecs::deserialize` mints fresh
    /// entities, and a restore that changed every identifier would be a divergence by the state
    /// hash's own definition.
    [[nodiscard]] Status restore(ecs::World& world,
                                 const determinism::StateProviderRegistry& registry) const noexcept;

    [[nodiscard]] SnapshotKind kind() const noexcept { return kind_; }
    [[nodiscard]] SnapshotEncoding encoding() const noexcept { return encoding_of(kind_); }
    [[nodiscard]] determinism::SimulationPoint point() const noexcept { return at_; }
    [[nodiscard]] bool captured() const noexcept { return captured_; }
    /// Bytes held, entity state and provider state together. What the rollback window's budget is
    /// measured in.
    [[nodiscard]] u64 bytes() const noexcept;
    [[nodiscard]] u32 providers_captured() const noexcept { return providers_captured_; }
    /// Providers that declined this kind. Counted, because a capture that skipped nine of ten
    /// providers and one that captured all ten read identically otherwise.
    [[nodiscard]] u32 providers_declined() const noexcept { return providers_declined_; }
    /// True when this kind stores an in-memory `ecs::Snapshot` rather than a byte stream. The
    /// structural half of "the save encoding SHALL NOT be used for rollback": a rollback capture
    /// has no stream and a save capture has no snapshot, and `tests/test_snapshot.cpp` checks both.
    [[nodiscard]] bool holds_in_memory_snapshot() const noexcept;
    [[nodiscard]] bool holds_tagged_stream() const noexcept;

    void clear() noexcept;

private:
    struct ProviderSlice {
        const char* name = "";
        u32 offset = 0;
        u32 size = 0;
    };

    ecs::Snapshot entities_;
    /// The tagged stream, for the two kinds that use one. Empty for the other two.
    Array<u8> stream_;
    /// Provider bytes, concatenated in the registry's finalised order, with a slice table.
    Array<u8> provider_bytes_;
    Array<ProviderSlice> providers_;
    determinism::SimulationPoint at_;
    SnapshotKind kind_ = SnapshotKind::Rollback;
    u32 providers_captured_ = 0;
    u32 providers_declined_ = 0;
    /// Bytes before compression, so `restore` knows what to expand to.
    u32 provider_bytes_plain_ = 0;
    bool compressed_ = false;
    bool captured_ = false;
};

// --- Checkpoint policy ---------------------------------------------------------------------------

/// What has happened since the last checkpoint. The policy's input.
struct CheckpointState {
    u64 ticks_since_last = 0;
    /// Bytes the last capture took. The policy uses it to keep a session inside its storage budget
    /// without being told the budget twice.
    u64 last_capture_bytes = 0;
    u64 commands_since_last = 0;
    /// Total bytes checkpoints already occupy.
    u64 storage_used = 0;
};

/// `replay-and-rollback`: "Checkpoint interval SHALL be a **policy** rather than a constant,
/// informed by: elapsed ticks, state size, command volume, expected seeking behaviour, and storage
/// budget."
///
/// Five inputs and not one, because a session with a small world and a lot of commands and one with
/// a huge world and none want opposite answers, and a constant gets one of them wrong.
struct CheckpointPolicy {
    /// Never more often than this, whatever else says.
    u32 min_tick_interval = 60;
    /// Always by this, unless the storage budget forbids it.
    u32 max_tick_interval = 600;
    /// Take one early when this many commands have accumulated: seeking cost is the *commands*
    /// between checkpoints, not the ticks.
    u64 command_volume_trigger = 4096;
    /// How far a viewer is expected to seek. A session nobody seeks in wants the loosest interval
    /// its budget allows; a coach's review tool wants the tightest.
    u64 expected_seek_ticks = 3600;
    /// Total bytes checkpoints may occupy. Zero means unbounded.
    u64 storage_budget = 0;

    /// Is a checkpoint due?
    [[nodiscard]] bool due(const CheckpointState& state) const noexcept;

    /// The interval this policy would choose given the last capture's size. Reported so a session
    /// can say what it decided and why, rather than a viewer discovering it by seeking.
    [[nodiscard]] u32 chosen_interval(const CheckpointState& state) const noexcept;
};

// --- The bounded rollback window
// ------------------------------------------------------------------

/// Why a rollback request could not be served.
enum class WindowRefusal : u8 {
    None = 0,
    /// Older than the window holds. `replay-and-rollback`: "a request older than the window SHALL
    /// be reported as requiring full resynchronisation rather than triggering an unbounded replay."
    RequiresResynchronisation,
    /// Ahead of everything captured.
    InTheFuture,
    /// Nothing has been captured at all.
    Empty,
};

const char* window_refusal_name(WindowRefusal refusal) noexcept;

/// An in-memory ring of rollback captures, bounded by a memory budget.
///
/// Bounded by **bytes** rather than by count, because that is the resource: a world that doubles in
/// size halves the window it can afford, and a ring of sixty captures would simply use twice the
/// memory and tell nobody.
class SnapshotRing {
public:
    SnapshotRing(Allocator& allocator, u64 memory_budget) noexcept
        : allocator_(&allocator), captures_(allocator), budget_(memory_budget) {}

    ~SnapshotRing();

    SnapshotRing(const SnapshotRing&) = delete;
    SnapshotRing& operator=(const SnapshotRing&) = delete;

    /// Capture `world` at `at`, evicting the oldest captures until the budget holds.
    ///
    /// Refuses only on allocation failure: a capture that does not fit the budget on its own
    /// shortens the window to one and reports it through `window_ticks()`, because refusing to
    /// capture at all would leave the session with no rollback rather than a short one.
    [[nodiscard]] Status capture(ecs::World& world,
                                 const determinism::StateProviderRegistry& registry,
                                 determinism::SimulationPoint at) noexcept;

    /// The capture at or before `at`, or a refusal saying which side of the window it fell off.
    [[nodiscard]] const StateCapture* find(determinism::SimulationPoint at,
                                           WindowRefusal& refusal) const noexcept;

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(captures_.size()); }
    [[nodiscard]] u64 bytes() const noexcept;
    [[nodiscard]] u64 budget() const noexcept { return budget_; }
    /// Ticks the window currently spans. The number a session reports when the budget shortened it.
    [[nodiscard]] u64 window_ticks() const noexcept;
    /// How many captures the budget has evicted. Reported: "the window SHALL shorten and the limit
    /// SHALL be reported".
    [[nodiscard]] u32 evictions() const noexcept { return evictions_; }

    void clear() noexcept;

private:
    void evict_to_budget() noexcept;

    Allocator* allocator_;
    Array<StateCapture*> captures_;
    u64 budget_ = 0;
    u32 evictions_ = 0;
};

}  // namespace cy::replay
