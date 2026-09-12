#pragma once
// The override layer, orphans, and the persistent delta. M10 task 4.2, whose second exit criterion
// is "a hand-placed override that survives regeneration of its region".
//
// `procedural-content-generation` — "Overrides and regeneration": "generated base + author
// overrides = authored result"; "Regeneration SHALL merge overrides by stable identity. An override
// whose target survives regeneration SHALL survive with it. An override whose target no longer
// exists SHALL become an ORPHAN: retained, reported, and resolvable — never silently discarded.
// Regenerating a region must not quietly delete a designer's work. Locked instances SHALL be
// preserved through regeneration."
//
// And "Persistence of generated content": "Generated base content SHALL NOT be saved. A save SHALL
// record the generator version and seed where needed, plus persistent exceptions: instances
// removed, modified, or added by gameplay or authoring ... Exceptions SHALL be anchored by stable
// identity AND SPATIALLY, so they can be re-resolved after a generator version change, and reported
// as orphaned when they cannot."
//
// ================================================================================================
// ONE LAYER, TWO AUTHORS
// ================================================================================================
//
// An author's moved tree and a player's felled tree are the same record with a different origin, so
// they are one mechanism here rather than two that have to agree. `OverrideOrigin` separates them
// for the one thing that differs: an AUTHORED override is content and travels with the project; a
// GAMEPLAY exception is save data and travels with the player. `PersistentDelta::save_records()`
// returns the second kind alone, which is the mechanical reading of "generated base content SHALL
// NOT be saved" — the base is not in the delta, and neither is the designer's work, which is in the
// project.
//
// ================================================================================================
// THE SPATIAL ANCHOR IS NOT A CONVENIENCE
// ================================================================================================
//
// design.md §1.4: under the DERIVED identity this module mints, 0 of 7 877 overrides mis-bound and
// 235 were LOST — the instances they named were genuinely removed by the edit. A lost override is
// visible and a mis-bound one is not, so re-resolution never guesses by identity: it re-binds by
// identity when the identity is still there, and otherwise falls back to the SPATIAL anchor within
// a declared tolerance and MARKS the result `Reanchored` rather than silently calling it a bind. An
// override that finds neither is an orphan, retained and reported.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/pcg/dataset.h>
#include <cy/pcg/identity.h>
#include <cy/pcg/invalidation.h>

namespace cy::pcg {

/// What an override does to the instance it names.
enum class OverrideOp : u8 {
    /// The instance is not part of the authored result. A felled tree, a deleted rock.
    Delete = 0,
    /// The instance keeps its identity and takes a new position.
    Move,
    /// The instance keeps its identity and takes a new variant — a different species, a different
    /// mesh. Carried as an integer the adapter interprets.
    Replace,
    /// An instance the generator did not produce. Its identity is minted from the override layer's
    /// own node rather than from the generator's, so it can never collide with a generated one.
    Add,
    /// The instance survives regeneration unchanged whatever the rules now say. "Locked instances
    /// SHALL be preserved through regeneration."
    Lock,
    /// One attribute takes a new value.
    SetAttribute,
    kCount,
};

[[nodiscard]] const char* override_op_name(OverrideOp op) noexcept;

/// Who wrote the override. See the header comment: it decides what a save contains.
enum class OverrideOrigin : u8 {
    /// A designer in the editor. Project content.
    Authored = 0,
    /// A player, at runtime. Save data.
    Gameplay,
};

/// One modification of the generated base.
struct Override {
    GeneratedId target;
    OverrideOp op = OverrideOp::Delete;
    OverrideOrigin origin = OverrideOrigin::Authored;

    /// The SPATIAL anchor: where the instance was when the override was written, in absolute world
    /// metres. Recorded for every override, not only for a move, because it is what re-resolution
    /// falls back to after a generator version change.
    f64 anchor_x = 0.0;
    f64 anchor_y = 0.0;
    f64 anchor_z = 0.0;

    /// `Move` and `Add`: the new position, in absolute world metres.
    f64 x = 0.0;
    f64 y = 0.0;
    f64 z = 0.0;

    /// `Replace` and `Add`: the variant the adapter resolves.
    u32 variant = 0;
    /// `SetAttribute`: which attribute, and to what.
    AttributeId attribute;
    f32 value = 0.0F;

    /// The generator version this override was written against. A difference is what puts it
    /// through re-resolution rather than through a plain bind.
    u32 written_against_version = 0;
};

/// How an override bound after a regeneration.
enum class BindResult : u8 {
    /// The identity was found. The ordinary case, and the only one that needs no report.
    Bound = 0,
    /// The identity was gone and the spatial anchor found an instance within tolerance. Recorded
    /// rather than treated as a bind, because it is a GUESS and the designer should see it.
    Reanchored,
    /// Neither. Retained and reported — never discarded.
    Orphaned,
};

[[nodiscard]] const char* bind_result_name(BindResult result) noexcept;

/// One override's fate, as a merge reports it.
struct BindReport {
    GeneratedId target;
    BindResult result = BindResult::Bound;
    OverrideOp op = OverrideOp::Delete;
    OverrideOrigin origin = OverrideOrigin::Authored;
    /// For `Reanchored`, the identity it was re-bound to and how far away it was. Both zero
    /// otherwise.
    GeneratedId reanchored_to;
    f32 distance_metres = 0.0F;
};

/// The result of merging overrides into a regenerated region.
///
/// `generated base + author overrides = authored result`, and this is the right-hand side plus the
/// report the equation does not mention.
struct MergeResult {
    explicit MergeResult(Allocator& allocator) noexcept : points(allocator), reports(allocator) {}

    MergeResult(const MergeResult&) = delete;
    MergeResult& operator=(const MergeResult&) = delete;
    MergeResult(MergeResult&&) noexcept = default;
    MergeResult& operator=(MergeResult&&) noexcept = default;

    PointSet points;
    Array<BindReport> reports;
    u32 bound = 0;
    u32 reanchored = 0;
    u32 orphaned = 0;
    u32 deleted = 0;
    u32 added = 0;
    u32 locked = 0;
};

/// The override layer over one generator's output.
class OverrideLayer {
public:
    explicit OverrideLayer(Allocator& allocator) noexcept
        : allocator_(&allocator), overrides_(allocator), index_(allocator), orphans_(allocator) {}

    OverrideLayer(const OverrideLayer&) = delete;
    OverrideLayer& operator=(const OverrideLayer&) = delete;

    /// Record an override. A second override of the same op on the same target replaces the first —
    /// a designer who moves a tree twice has moved it once, to the second place.
    [[nodiscard]] Status place(const Override& record) noexcept;
    [[nodiscard]] bool remove(GeneratedId target, OverrideOp op) noexcept;

    [[nodiscard]] Span<const Override> records() const noexcept { return overrides_.span(); }
    [[nodiscard]] usize size() const noexcept { return overrides_.size(); }

    /// Merge into a regenerated point set. `tolerance_metres` is how far a spatial re-anchor may
    /// reach; zero disables re-anchoring entirely, which is the right setting for a regeneration
    /// that did NOT change the generator version.
    ///
    /// `origin_x`/`origin_z` convert the point set's region-local positions to absolute metres, so
    /// that an anchor written in world space and a point stored in region space are compared in one
    /// frame rather than in whichever the caller had to hand.
    [[nodiscard]] Expected<MergeResult, Error> merge(const PointSet& base, f64 origin_x,
                                                     f64 origin_z, f32 tolerance_metres,
                                                     u32 current_version) const noexcept;

    /// Overrides that found nothing in the last merge. Retained across merges, because an orphan
    /// whose instance comes back — a rule change reverted — should bind again rather than have been
    /// thrown away.
    [[nodiscard]] Span<const BindReport> orphans() const noexcept { return orphans_.span(); }
    [[nodiscard]] Status refresh_orphans(const MergeResult& result) noexcept;

private:
    [[nodiscard]] const Override* find(GeneratedId target, OverrideOp op) const noexcept;

    Allocator* allocator_;
    Array<Override> overrides_;
    HashMap<u64, usize> index_;
    Array<BindReport> orphans_;
};

// --- Persistence ---------------------------------------------------------------------------------

/// What a save records. "generated base + persistent delta = current procedural world."
struct PersistentDelta {
    explicit PersistentDelta(Allocator& allocator) noexcept : exceptions(allocator) {}

    PersistentDelta(const PersistentDelta&) = delete;
    PersistentDelta& operator=(const PersistentDelta&) = delete;
    PersistentDelta(PersistentDelta&&) noexcept = default;
    PersistentDelta& operator=(PersistentDelta&&) noexcept = default;

    /// The seed and version the exceptions were written against. "A save SHALL record the generator
    /// version and seed where needed" — needed exactly so that re-resolution can tell a version
    /// change from an ordinary load.
    u64 seed = 0;
    u32 generator_version = 0;
    u64 program_digest = 0;

    Array<Override> exceptions;

    /// Bytes a save of this delta occupies, as `encode()` writes them. The number that makes "a
    /// cleared forest saves cheaply" measurable: it is a function of the exception count and of
    /// nothing about the forest.
    [[nodiscard]] u64 encoded_bytes() const noexcept;
};

/// Extract the gameplay-origin exceptions from an override layer. Authored overrides are project
/// content and are NOT in a save; the generated base is in neither.
[[nodiscard]] Status collect_persistent(const OverrideLayer& layer, u64 seed, u32 version,
                                        u64 program_digest, PersistentDelta& out) noexcept;

/// Re-resolve a delta against a world generated by a DIFFERENT generator version.
///
/// "WHEN a generator version changes THEN exceptions SHALL be re-resolved where possible and
/// reported where not." Identity first, spatial anchor second, orphan third — and the second is
/// reported as `Reanchored` rather than folded into the first, for the reason the header gives.
///
/// `current_version` is the version the world in front of us was generated by. Passing the delta's
/// own version means "nothing changed", and the spatial fallback stays shut — which is correct: a
/// missing identity under an unchanged version is a removal, not a rename.
[[nodiscard]] Expected<MergeResult, Error> re_resolve(Allocator& allocator,
                                                      const PersistentDelta& delta,
                                                      const PointSet& regenerated, f64 origin_x,
                                                      f64 origin_z, u32 current_version,
                                                      f32 tolerance_metres) noexcept;

}  // namespace cy::pcg
