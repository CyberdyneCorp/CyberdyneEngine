#pragma once
// The priority scheduler, network level of detail, and the bandwidth budget. M9 task 4.4.
//
// ================================================================================================
// THE DEGRADATION ORDER IS A REQUIREMENT, IN THIS ORDER, AND IT IS CHECKED
// ================================================================================================
//
// `networking-and-replication` — "Bandwidth management": "Budget enforcement SHALL degrade in a
// defined order: reduce update frequency for low-priority bands, reduce encoding precision, then
// mark entities dormant — before omitting updates entirely."
//
// Four steps, and the order matters because the first three are invisible to a player and the
// fourth is not. `select()` applies them in that order and reports how many entities each step
// touched, so `tests/test_scheduler.cpp` can assert that a budget squeeze produced frequency
// reductions before it produced omissions — rather than asserting that the total fitted, which a
// scheduler that simply dropped the tail would also satisfy.
//
// ================================================================================================
// BOUNDED STALENESS IS WHAT STOPS "LOW PRIORITY" MEANING "NEVER"
// ================================================================================================
//
// "The scheduler SHALL guarantee **bounded staleness** per band, so a low-priority entity is never
// starved indefinitely, and staleness SHALL feed back into the score." Two mechanisms, both here:
// staleness is a term of the score, *and* an entity past its band's guaranteed interval is marked
// `must_send`, which places it ahead of everything that is not. The second exists because the first
// alone is a race that a sufficiently crowded tick always loses.
//
// ================================================================================================
// WHY THE SCORE IS AN INTEGER
// ================================================================================================
//
// The same argument `interest.h` makes about relevance distance. A priority computed in floating
// point is a priority two builds can order differently, and a lockstep session compares the result
// of what was sent. Every term below is `u32` arithmetic with a stated weight, and ties break on
// the network id — `determinism::sort_by_key`'s own rule, applied through a total-order comparator.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/networking/interest.h>

namespace cy::net {

/// `networking-and-replication`'s network level-of-detail table.
enum class NetworkLodBand : u8 {
    OwnedOrSelected = 0,
    NearChanging,
    NearIdle,
    Far,
    VeryFar,
    /// Nothing has changed; the peer has been told so and expects silence.
    Dormant,
};

inline constexpr u32 kNetworkLodBandCount = 6;

const char* network_lod_band_name(NetworkLodBand band) noexcept;

/// One band's policy, in ticks rather than hertz: the scheduler is driven by a fixed timestep and a
/// frequency in hertz would have to be divided by one at every comparison.
struct BandPolicy {
    /// Ticks between updates in this band, at no bandwidth pressure.
    u32 interval_ticks = 1;
    /// Encoding precision as a percentage of the schema's declared bits. 100 is full.
    u32 precision_percent = 100;
    /// The longest this band may go without an update whatever the pressure. Bounded staleness.
    u32 guaranteed_interval_ticks = 1;
};

/// The default table, at the engine's 60 Hz simulation tick. The specification's own frequencies:
/// owned 30-60 Hz, near-changing 20 Hz, near-idle 10 Hz, far 2 Hz, very far 0.2 Hz.
struct BandTable {
    BandPolicy policies[kNetworkLodBandCount] = {
        {1, 100, 2},     // OwnedOrSelected  — every tick
        {3, 100, 8},     // NearChanging     — 20 Hz
        {6, 75, 20},     // NearIdle         — 10 Hz, reduced
        {30, 50, 120},   // Far              — 2 Hz, coarse
        {300, 25, 900},  // VeryFar          — 0.2 Hz, minimal
        {0, 0, 0},       // Dormant          — nothing until it changes
    };

    [[nodiscard]] const BandPolicy& of(NetworkLodBand band) const noexcept {
        return policies[static_cast<u32>(band)];
    }
};

/// The per-peer outgoing budget. `networking-and-replication`: "Budgets SHALL be settable per peer,
/// so a peer on a constrained connection can be served differently without changing the
/// simulation."
struct BandwidthBudget {
    /// Bytes the peer may be sent this tick, application payload only.
    u32 bytes_per_tick = 4096;
    /// The largest datagram, to avoid IP fragmentation.
    u32 mtu = kDefaultMtu;
    /// Ticks of no change before an entity becomes dormant.
    u32 dormant_after_ticks = 120;
};

/// The weights of the score. Data rather than constants in the function, so a project can retune
/// without a fork and so a test can isolate one term by zeroing the others.
struct PriorityWeights {
    u32 ownership = 10000;
    u32 importance = 100;
    u32 recency_of_change = 400;
    u32 staleness_per_tick = 8;
    /// Subtracted, scaled by the distance band the candidate fell into.
    u32 distance_penalty = 200;
    /// Added when the peer can see it. Visibility is the caller's to decide; the scheduler only
    /// scores it.
    u32 visibility = 1500;
};

/// One entity the scheduler selected, and what it decided about it.
struct ScheduledEntry {
    NetworkId id;
    u32 score = 0;
    NetworkLodBand band = NetworkLodBand::Far;
    /// Percentage of the schema's declared bits to spend. Below 100 after a precision reduction.
    u32 precision_percent = 100;
    /// The bytes this entity was budgeted, from the caller's estimate.
    u32 bytes = 0;
    /// True when this update is the "you are dormant" signal rather than state.
    /// `networking-and-replication`: "dormancy explicitly signalled so the client does not treat
    /// them as lost".
    bool dormancy_signal = false;
    /// True when the band's guaranteed interval forced it in. Bounded staleness, as a flag a test
    /// can count.
    bool forced_by_staleness = false;
};

/// What one `select()` did. Every step of the degradation order has a number here.
struct SchedulerReport {
    u32 candidates = 0;
    u32 selected = 0;
    /// Not sent this tick because their band's interval has not elapsed. Not a degradation.
    u32 not_due = 0;
    u32 frequency_reduced = 0;
    u32 precision_reduced = 0;
    u32 marked_dormant = 0;
    /// Selected, deferred by the budget, and still owed. The backlog.
    u32 deferred = 0;
    u32 forced_by_staleness = 0;
    u64 bytes_planned = 0;
    u64 bytes_budget = 0;
};

/// How many bytes an entity's update would cost. The scheduler does not encode, so the cost is the
/// caller's to estimate — from `CompiledSchema::bits_for()`, which is where the real number is.
using CostFn = u32 (*)(void* user, NetworkId id, u32 precision_percent) noexcept;

/// The scheduler. One per session; peers are distinguished inside it.
class PriorityScheduler {
public:
    explicit PriorityScheduler(Allocator& allocator) noexcept;
    ~PriorityScheduler();

    PriorityScheduler(const PriorityScheduler&) = delete;
    PriorityScheduler& operator=(const PriorityScheduler&) = delete;

    void set_weights(const PriorityWeights& weights) noexcept { weights_ = weights; }
    void set_bands(const BandTable& bands) noexcept { bands_ = bands; }
    [[nodiscard]] const BandTable& bands() const noexcept { return bands_; }

    /// Tell the scheduler an entity's state changed at `tick`. What "recency of change" and
    /// dormancy are both computed from.
    [[nodiscard]] Status note_changed(NetworkId id, u64 tick) noexcept;

    /// Tell the scheduler an entity was actually sent to a peer. Called after the packet is built,
    /// because an entity that was scheduled and then did not fit must stay stale.
    [[nodiscard]] Status note_sent(PeerId peer, NetworkId id, u64 tick) noexcept;

    /// Mark an entity as visible to a peer. Optional; absent means not visible, which costs the
    /// visibility term and nothing else.
    [[nodiscard]] Status note_visible(PeerId peer, NetworkId id, bool visible) noexcept;

    /// Choose what `peer` is sent this tick. `candidates` is `InterestSet::evaluate()`'s output.
    [[nodiscard]] Expected<SchedulerReport, Error> select(PeerId peer,
                                                          Span<const Candidate> candidates,
                                                          u64 tick, const BandwidthBudget& budget,
                                                          CostFn cost, void* user,
                                                          Array<ScheduledEntry>& out) noexcept;

    /// The band an entity falls into for a peer, before any degradation. Exposed because the
    /// profiler answers "why was this replicated" with it.
    [[nodiscard]] NetworkLodBand band_for(PeerId peer, const Candidate& candidate,
                                          u64 tick) const noexcept;

    /// Distance thresholds, squared, in the same units as `RelevanceSubject::position_*`.
    void set_distance_bands(i64 near_squared, i64 far_squared) noexcept {
        near_squared_ = near_squared;
        far_squared_ = far_squared;
    }

private:
    struct EntityState {
        u64 last_changed_tick = 0;
        bool dormant = false;
    };
    struct PeerEntityState {
        u64 last_sent_tick = 0;
        bool visible = false;
        bool dormancy_told = false;
    };
    struct PeerState {
        PeerId peer;
        HashMap<u64, PeerEntityState> entities;

        explicit PeerState(Allocator& allocator) noexcept : entities(allocator) {}
    };

    [[nodiscard]] Expected<PeerState*, Error> state_for(PeerId peer) noexcept;
    [[nodiscard]] const PeerState* state_for(PeerId peer) const noexcept;
    [[nodiscard]] u32 score_of(const Candidate& candidate, const EntityState& entity,
                               const PeerEntityState& per_peer, u64 tick) const noexcept;

    Allocator* allocator_;
    HashMap<u64, EntityState> entities_;
    Array<PeerState*> peers_;
    Array<ScheduledEntry> scratch_;
    PriorityWeights weights_{};
    BandTable bands_{};
    i64 near_squared_ = 2500;
    i64 far_squared_ = 250000;
};

}  // namespace cy::net
