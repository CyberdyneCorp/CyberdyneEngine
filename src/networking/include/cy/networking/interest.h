#pragma once
// Interest management: relevance produces candidates, incrementally. M9 task 4.4.
//
// ================================================================================================
// RELEVANCE IS A CANDIDATE SET, NOT A DECISION
// ================================================================================================
//
// `networking-and-replication` is explicit and the distinction is the whole design:
// "**Interest management is a scheduler, not a predicate.** Relevance produces candidates; a
// priority scheduler then decides what is sent this tick, at what frequency, and at what precision,
// within a bandwidth budget."
//
// So nothing here sends anything, scores anything or looks at a budget. `InterestSet::evaluate()`
// answers "which entities may this peer be told about", and `scheduler.h` answers "which of those
// are told about now". Two objects rather than one because the failure of putting them together is
// that a bandwidth budget quietly becomes a visibility rule, and a player stops seeing an enemy
// because the network was busy.
//
// ================================================================================================
// CELLS COME FROM THE WORLD PARTITION. NETWORKING KEEPS NO SECOND SPATIAL MODEL
// ================================================================================================
//
// "Replication cells SHALL derive from the **world partition** ... Networking SHALL NOT maintain a
// second spatial subdivision of the same world." There is therefore no quadtree, no grid and no
// octree in this file. A `CellId` is an opaque number the world partition hands out; this module
// stores membership and nothing else, and `aggregate()` is the one derivation it is allowed —
// coarsening world cells into replication cells **by arithmetic on the world's own identifier**, so
// that client streaming and server relevance still refer to the same content by the same number.
//
// ================================================================================================
// INCREMENTAL, BECAUSE THE ALTERNATIVE IS QUADRATIC
// ================================================================================================
//
// "Relevance SHALL be evaluated incrementally where possible, using cell membership changes rather
// than re-evaluating every entity against every peer each tick." `evaluate()` walks the peer's
// interest cells and the entities in them; it never walks the world. `tests/test_interest.cpp`
// counts the entities examined against the world's population and fails if the count scales with
// the world rather than with the peer's cells — which is the requirement as a measurement, and the
// thing a comment cannot check.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/networking/authority.h>
#include <cy/networking/transport.h>

namespace cy::net {

/// A world-partition cell, as the world partition numbers it. Opaque here — see the header comment.
using CellId = u64;
inline constexpr CellId kNoCell = ~0ULL;

/// Why an entity is a candidate for a peer. Reported so the profiler can answer "why was this
/// replicated" with the rule rather than with a yes.
enum class RelevanceRule : u8 {
    None = 0,
    /// The peer owns it.
    Ownership,
    /// Marked always relevant — an objective, a match timer.
    AlwaysRelevant,
    /// In one of the peer's interest cells.
    CellMembership,
    /// Within the peer's relevance distance. Checked after the cell, and only for entities the cell
    /// already admitted.
    Distance,
    /// Same team or faction.
    Team,
    /// A project-supplied predicate said so.
    CustomPredicate,
};

const char* relevance_rule_name(RelevanceRule rule) noexcept;

/// What replication knows about one entity for the purpose of relevance. Deliberately a flat value
/// rather than a component query: this module has no world, and `tests/` can therefore drive ten
/// thousand entities without one.
struct RelevanceSubject {
    NetworkId id;
    CellId cell = kNoCell;
    PeerId owner;
    u32 team = 0;
    /// Squared distance is compared, so nothing here takes a square root.
    i64 position_x = 0;
    i64 position_y = 0;
    i64 position_z = 0;
    bool always_relevant = false;
    /// Gameplay importance, passed through to the scheduler's score untouched.
    u32 importance = 0;
};

/// What a peer is interested in.
struct PeerInterest {
    PeerId peer;
    i64 viewpoint_x = 0;
    i64 viewpoint_y = 0;
    i64 viewpoint_z = 0;
    /// Squared, in the same units as the subject's position.
    i64 relevance_distance_squared = 0;
    u32 team = 0;
    /// Team membership admits an entity regardless of distance — a squad marker.
    bool team_is_relevant = false;
};

/// A project's own rule. Returns true to admit.
using RelevancePredicate = bool (*)(void* user, const PeerInterest& peer,
                                    const RelevanceSubject& subject) noexcept;

/// One candidate, with the rule that admitted it.
struct Candidate {
    NetworkId id;
    RelevanceRule rule = RelevanceRule::None;
    i64 distance_squared = 0;
    u32 importance = 0;
};

/// What changed in a peer's candidate set between two evaluations.
///
/// "Entities entering relevance SHALL receive a baseline; entities leaving SHALL be explicitly
/// dropped so the client can clean up." Both halves are reported here, because a client left with a
/// frozen ghost is a bug a player reports and a server never sees.
struct RelevanceDelta {
    u32 entered = 0;
    u32 left = 0;
};

/// The cell index and the per-peer candidate sets.
class InterestSet {
public:
    explicit InterestSet(Allocator& allocator) noexcept;

    InterestSet(const InterestSet&) = delete;
    InterestSet& operator=(const InterestSet&) = delete;

    /// Add or move an entity. Moving is what makes evaluation incremental: the index is updated
    /// when the entity's cell changes rather than rebuilt when a peer moves.
    [[nodiscard]] Status place(const RelevanceSubject& subject) noexcept;
    void remove(NetworkId id) noexcept;

    /// Coarsen a world cell into a replication cell. `factor` is a power of two and the derivation
    /// is division, so the replication cell is a function of the world cell's own identity — see
    /// the header comment.
    [[nodiscard]] static constexpr CellId aggregate(CellId world_cell, u32 shift) noexcept {
        return world_cell == kNoCell ? kNoCell : (world_cell >> shift);
    }

    /// Set a peer's interest cells. Taken by value and copied, because the caller's list is the
    /// world partition's and may be rebuilt under us.
    [[nodiscard]] Status set_interest_cells(PeerId peer, Span<const CellId> cells) noexcept;

    void set_predicate(RelevancePredicate predicate, void* user) noexcept {
        predicate_ = predicate;
        predicate_user_ = user;
    }

    /// The candidate set for `peer`, appended to `out`. Also reports what entered and left since
    /// the previous evaluation for the same peer.
    [[nodiscard]] Expected<RelevanceDelta, Error> evaluate(const PeerInterest& peer,
                                                           Array<Candidate>& out,
                                                           Array<NetworkId>& left) noexcept;

    /// Entities examined by the last `evaluate()`. The number `tests/test_interest.cpp` compares
    /// against the world's population: relevance that scaled with the world rather than with the
    /// peer's cells would show here rather than as a frame time six months later.
    [[nodiscard]] u64 examined_last() const noexcept { return examined_last_; }
    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(subjects_.size()); }
    [[nodiscard]] u32 cell_count() const noexcept { return static_cast<u32>(cells_.size()); }

private:
    struct CellBucket {
        CellId cell = kNoCell;
        Array<u32> members;

        explicit CellBucket(Allocator& allocator) noexcept : members(allocator) {}
    };
    struct PeerState {
        PeerId peer;
        Array<CellId> cells;
        /// The previous evaluation's candidate ids, and this one's. Hash sets rather than arrays
        /// because the entered/left comparison is otherwise quadratic in the candidate count, and
        /// the whole point of task 4.4 is a curve measured against a population that grows.
        HashSet<u64> previous;
        HashSet<u64> current;

        explicit PeerState(Allocator& allocator) noexcept
            : cells(allocator), previous(allocator), current(allocator) {}
    };

    [[nodiscard]] Expected<u32, Error> bucket_for(CellId cell) noexcept;
    [[nodiscard]] Expected<PeerState*, Error> peer_state(PeerId peer) noexcept;
    [[nodiscard]] RelevanceRule admits(const PeerInterest& peer, const RelevanceSubject& subject,
                                       i64& distance_squared) const noexcept;
    [[nodiscard]] Status consider(const PeerInterest& peer, u32 subject_index,
                                  Array<Candidate>& out) noexcept;

    Allocator* allocator_;
    Array<RelevanceSubject> subjects_;
    HashMap<u64, u32> by_id_;
    Array<CellBucket*> cells_;
    HashMap<u64, u32> cell_index_;
    Array<u32> always_relevant_;
    Array<PeerState*> peers_;
    RelevancePredicate predicate_ = nullptr;
    void* predicate_user_ = nullptr;
    u64 examined_last_ = 0;

public:
    ~InterestSet();
};

}  // namespace cy::net
