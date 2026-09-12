#pragma once
// Exceptions: the only thing a world stores about its foliage. M10 task 2.4.
//
// `foliage` — "Exceptions are stored, instances are not": "Only EXCEPTIONS to procedural placement
// SHALL be stored ... Exceptions SHALL be anchored by stable instance identity AND SPATIALLY, so
// that after a rule graph or seed change they can be re-resolved against the regenerated set. An
// exception that cannot be re-resolved SHALL become an ORPHANED EXCEPTION: retained, reported, and
// resolvable, and SHALL NOT be silently discarded — regenerating a region must not quietly undo
// deliberate work."
//
// ================================================================================================
// TWO ANCHORS, AND WHY NEITHER ALONE IS ENOUGH
// ================================================================================================
//
// IDENTITY alone survives a regeneration of the same seed and rule version and nothing else: the
// version participates in `cluster_identity()`, so an edit to the rule graph renames every cluster
// and every identity with it, and an exception anchored only by identity would be orphaned by a
// change that did not move the tree it refers to.
//
// SPATIAL alone re-binds after a rule change and mis-binds before one: two saplings a metre apart
// are the same position to within any tolerance worth using, and the M10 spike's override table is
// what a scheme that binds by proximity looks like when it is wrong — 3 351 of 7 877 overrides
// silently moved to a DIFFERENT object, which is worse than losing them, because nothing reports
// it.
//
// So `resolve()` tries identity first and falls back to a spatial match that is constrained by
// species AND by a declared radius AND by uniqueness: two equally-close candidates of the right
// species are an AMBIGUOUS match and produce an orphan, not a coin toss. An orphan is a thing an
// author fixes; a wrong binding is a thing nobody ever finds.
//
// ================================================================================================
// PERSISTENCE GOES THROUGH THE WORLD'S OWN OVERLAY
// ================================================================================================
//
// "Exceptions SHALL be recorded in the world persistence overlay where they are runtime changes,
// and in authoring data where they are authored." `world::PersistenceOverlay::record_blob()` is
// the mechanism `src/terrain/` opened for exactly this shape of subsystem state, and
// `kOverlayChannelFoliage` is this module's channel in it. `ExceptionKind::authored()` is the line
// between the two halves: an authored exception is cooked and is not written to a save.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/foliage/instance.h>
#include <cy/foliage/species.h>
#include <cy/world/overlay.h>

namespace cy::foliage {

/// This module's channel in `world::PersistenceOverlay`. `terrain::kOverlayChannelTerrain` is 1;
/// the numbers are small integers each subsystem picks once, and they are listed together nowhere
/// because the overlay deliberately does not interpret them.
inline constexpr u32 kOverlayChannelFoliage = 2;

/// What an exception says happened to an instance.
enum class ExceptionKind : u8 {
    /// The rules produced it and something removed it — the player felled it, an author deleted it.
    Removed = 0,
    /// The rules produced it and something moved, rotated or rescaled it.
    Moved,
    /// An author or gameplay added an instance the rules did not produce.
    Added,
    /// The rules produced it and it still stands, but its state changed — felled, burned,
    /// harvested.
    Modified,
};

[[nodiscard]] const char* exception_kind_name(ExceptionKind kind) noexcept;

/// Whether this kind of exception is AUTHORING data (cooked, never saved) or a RUNTIME change
/// (written to the persistence overlay). The distinction is the source of the exception rather than
/// its kind, so it lives on the record; this is the default for a kind and `FoliageException::
/// authored` is what actually decides.
[[nodiscard]] bool authored_by_default(ExceptionKind kind) noexcept;

/// One exception. Deliberately larger than an instance: there are fifty of these where there are a
/// hundred thousand instances, and every byte here buys re-resolution after a rule change.
struct FoliageException {
    ExceptionKind kind = ExceptionKind::Removed;
    /// The identity anchor. Zero for an `Added` exception, which names no generated instance.
    InstanceId identity;
    /// The cluster the exception was recorded against. Carried so a save can be organised by
    /// cell without re-deriving it, and so a diagnostic can name the region.
    ClusterId cluster;
    /// The spatial anchor: where the instance was when the exception was recorded.
    world::WorldVec3d position;
    /// The species anchor. A spatial re-bind that ignored species would bind a felled oak to a
    /// fern.
    SpeciesId species;

    /// For `Moved` and `Added`: the instance's new state, in the same compact form a generated one
    /// has. Quantised against the CLUSTER that holds it, so applying it is a copy.
    FoliageInstance instance;

    /// For `Modified`: the flags gameplay set — felled, burned.
    InstanceFlags state;

    /// Authoring data rather than a runtime change. Cooked; not written to a save.
    bool authored = false;

    /// A monotonically increasing stamp, so that two exceptions on one instance resolve to the
    /// later one deterministically rather than by array order.
    u64 sequence = 0;
};

/// What re-resolution did to one exception.
enum class ResolutionOutcome : u8 {
    /// Its identity was found in the regenerated set. The ordinary case.
    BoundByIdentity = 0,
    /// Its identity was gone and exactly one same-species instance was within the radius.
    BoundBySpace,
    /// It names no generated instance and needs none — an `Added` exception.
    Standalone,
    /// Nothing matched. Retained and reported; never dropped.
    Orphaned,
    /// Two or more equally good spatial matches. Also an orphan — see the header note.
    Ambiguous,
};

[[nodiscard]] const char* resolution_outcome_name(ResolutionOutcome outcome) noexcept;

/// One exception's fate, and what it bound to.
struct Resolution {
    ResolutionOutcome outcome = ResolutionOutcome::Orphaned;
    /// The slot in the regenerated cluster, or `FoliageCluster::kNoSlot`.
    u32 slot = FoliageCluster::kNoSlot;
    /// Metres between the anchor and what it bound to. Zero for an identity binding.
    f32 drift_metres = 0.0F;
};

/// Counts for a whole re-resolution. `foliage`'s diagnostics requirement asks for "exception counts
/// and ORPHANED EXCEPTIONS", and the spike's override table is the number this has to beat: 7 877
/// overrides, 0 mis-bound under a derived identity.
struct ResolutionReport {
    u32 examined = 0;
    u32 bound_by_identity = 0;
    u32 bound_by_space = 0;
    u32 standalone = 0;
    u32 orphaned = 0;
    u32 ambiguous = 0;

    [[nodiscard]] u32 orphans() const noexcept { return orphaned + ambiguous; }
};

/// How a spatial re-bind is allowed to work. A declaration rather than a constant, because the
/// radius that is right for a forest of oaks is wrong for a meadow.
struct ResolutionPolicy {
    /// The largest distance a spatial re-bind may cross, in metres.
    f32 radius_metres = 3.0F;
    /// Two candidates whose distances differ by less than this are AMBIGUOUS. Without it, a
    /// re-bind is decided by floating-point noise between two saplings.
    f32 ambiguity_metres = 0.25F;
    /// Whether a spatial re-bind is allowed at all. A project that would rather see orphans than
    /// risk a wrong binding sets this false and gets exactly that.
    bool allow_spatial = true;
};

/// The exceptions of a world, by cluster.
///
/// Keyed by cluster rather than by instance because that is how they are streamed, saved and
/// re-resolved: a region is regenerated and its exceptions are applied to it, and an index over
/// every exception in the world would be an index nothing ever queries.
class ExceptionStore {
public:
    explicit ExceptionStore(Allocator& allocator) noexcept;

    ExceptionStore(const ExceptionStore&) = delete;
    ExceptionStore& operator=(const ExceptionStore&) = delete;

    /// Record one exception. Recording a second on the same (cluster, identity, kind) REPLACES the
    /// first, keeping the higher sequence — so felling a tree twice is one exception and a replay
    /// that re-applies an event does not grow the save.
    [[nodiscard]] Status record(const FoliageException& exception) noexcept;

    [[nodiscard]] Span<const FoliageException> of_cluster(ClusterId cluster) const noexcept;
    /// Every exception in the store, across every cluster. The number the "fifty exceptions, not a
    /// hundred thousand instances" scenario is about.
    [[nodiscard]] usize size() const noexcept;
    [[nodiscard]] usize cluster_count() const noexcept { return exceptions_.size(); }

    /// Bytes the store holds. `foliage` — "WHEN a player fells fifty trees in a forest of a hundred
    /// thousand THEN the save SHALL record fifty exceptions, not a hundred thousand instances" —
    /// and this against `ClusterStore::bytes()` is that sentence measured.
    [[nodiscard]] u64 bytes() const noexcept;

    /// Re-resolve one cluster's exceptions against a regenerated cluster, writing one `Resolution`
    /// per exception in the same order `of_cluster()` returns them.
    ///
    /// The regenerated cluster may have a DIFFERENT identity from the one the exceptions were
    /// recorded against — that is the rule-change case, and `old_cluster` is what they were
    /// recorded under.
    [[nodiscard]] Expected<ResolutionReport, Error> resolve(u64 seed, ClusterId old_cluster,
                                                            const FoliageCluster& regenerated,
                                                            const ResolutionPolicy& policy,
                                                            Array<Resolution>& out) const noexcept;

    /// Apply resolved exceptions to a cluster: flags for `Removed` and `Modified`, a replaced
    /// record for `Moved`. `Added` exceptions are NOT applied here — they change the cluster's
    /// instance count and therefore its slots, so they are applied by the builder before the
    /// cluster is finished (`apply_added()`).
    [[nodiscard]] Expected<u32, Error> apply(ClusterId old_cluster, Span<const Resolution> resolved,
                                             FoliageCluster& cluster) const noexcept;

    /// Exceptions the last `resolve()` could not bind, for the editor's own list. Recomputed from
    /// the resolutions the caller holds, so the store does not carry per-resolution state.
    [[nodiscard]] Status orphans(ClusterId cluster, Span<const Resolution> resolved,
                                 Array<FoliageException>& out) const noexcept;

    // --- Persistence
    // ------------------------------------------------------------------------------

    /// Write one cluster's RUNTIME exceptions into the world's persistence overlay, as one blob
    /// under `kOverlayChannelFoliage` keyed by the cluster. Authored exceptions are skipped: they
    /// are cooked, and writing them to a save would grow every save by the whole authored set.
    ///
    /// Returns how many exceptions were written.
    [[nodiscard]] Expected<u32, Error> write_overlay(world::PersistenceOverlay& overlay,
                                                     world::CellId cell,
                                                     ClusterId cluster) const noexcept;

    /// Read one cluster's exceptions back out of an overlay, appending to this store.
    [[nodiscard]] Expected<u32, Error> read_overlay(const world::PersistenceOverlay& overlay,
                                                    world::CellId cell, ClusterId cluster) noexcept;

    /// Every cluster the store holds exceptions for, in a canonical order. A save's iteration.
    [[nodiscard]] Status clusters(Array<ClusterId>& out) const noexcept;

private:
    struct Bucket {
        ClusterId cluster;
        Array<FoliageException> items;

        explicit Bucket(Allocator& allocator) noexcept : items(allocator) {}
    };

    [[nodiscard]] Expected<Bucket*, Error> bucket_for(ClusterId cluster) noexcept;
    [[nodiscard]] const Bucket* find_bucket(ClusterId cluster) const noexcept;

    Allocator* allocator_;
    Array<Bucket> exceptions_;
    HashMap<u64, usize> index_;
};

/// Add every `Added` exception of a cluster into a builder before it is finished.
///
/// Separate from `ExceptionStore::apply()` because an added instance changes the cluster's slot
/// numbering, so it has to happen while the slots are still being decided. That it is a free
/// function taking a builder is the shape that makes it impossible to call at the wrong time.
[[nodiscard]] Expected<u32, Error> apply_added(const ExceptionStore& store, ClusterId cluster,
                                               const ClusterBounds& bounds,
                                               ClusterBuilder& builder) noexcept;

}  // namespace cy::foliage
