#pragma once
// Stable generated identity. M10 task 4.2, whose exit criterion this file IS.
//
// `procedural-content-generation` — "Stable generated identity": "Every generated output SHALL
// carry a stable identity derived from generator, region, and a stable per-output key. Identity
// SHALL NOT derive from iteration order, array index, insertion order, or worker scheduling."
//
// ================================================================================================
// THE SPIKE MEASURED THE THREE SCHEMES AND ONLY ONE OF THEM SURVIVES AN EDIT
// ================================================================================================
//
// design.md §1.4. 7 877 hand-placed overrides, rebound by identity across twelve regenerations:
//
//   counter  4 521 bound,   5 lost, 3 351 MIS-BOUND (43%)
//   rank     7 515 bound,  70 lost,   292 MIS-BOUND (3.7%)
//   derived  7 642 bound, 235 lost,     0 MIS-BOUND
//
// A LOST override is visible: the instance it named is genuinely gone because the edit removed it,
// and `overrides.h` reports it as an orphan. A MIS-BOUND override silently moves a DIFFERENT
// object, and nothing in the world reports it — which `counter` does to 43% of overrides on an
// ordinary FULL regeneration, before partial regeneration is involved at all.
//
// So `IdentitySource` below names all three, and `program.h`'s compiler REFUSES the two that lose.
// The alternative — offering only the sound one — would make the refusal unwritable and the
// requirement a convention again; the shape here is the one `environment-fields` uses for a second
// producer, and M8.c's cook-time refusal before that.
//
// ================================================================================================
// SUBSTREAM THEN DRAW, AND THE DEFECT THE SPIKE FOUND IN ITSELF
// ================================================================================================
//
// design.md §1.6 records it beside the fix because it looked identical to a failure of the thing
// being measured: `Derived` was first `fold_multiply(region + 1, slot + 1)`, which for small
// operands is plain `a * b` — the product's high half is zero, so there is nothing to fold — and
// `a * b` is not injective, so region 1 slot 5 and region 5 slot 1 were ONE instance. It reported
// 93 of 455 overrides mis-bound under a scheme that cannot mis-bind.
//
// `derive_identity()` is therefore `substream` twice and then `draw`, which is `random.h`'s own
// hierarchical derivation and two mixing rounds per level rather than one multiply. AN IDENTITY
// SCHEME IS ONLY AS STABLE AS THE FUNCTION DERIVING IT, and tests/test_identity.cpp holds that as a
// case: 262 144 (region, slot) pairs, zero collisions, and the same suite goes red against the
// multiply.

#include <cy/core/base/types.h>
#include <cy/core/determinism/random.h>
#include <cy/core/memory/hash.h>

namespace cy::pcg {

/// A generated instance's identity. Opaque: an override, a save exception, a provenance record and
/// a network delta all name an instance by this and by nothing else.
struct GeneratedId {
    u64 value = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(GeneratedId, GeneratedId) noexcept = default;
    friend constexpr bool operator<(GeneratedId a, GeneratedId b) noexcept {
        return a.value < b.value;
    }
};

/// A node's identity within a compiled program. Derived from the node's authored NAME rather than
/// from its index, so that inserting a node upstream does not renumber every instance downstream of
/// it — the graph-level version of the same mistake `Counter` makes at the instance level.
struct NodeIdentity {
    u64 value = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(NodeIdentity, NodeIdentity) noexcept = default;
};

/// A region's identity, as identity derivation keys on it. Three integers folded once, so that two
/// regions at different levels of the generation hierarchy are distinct subjects.
struct RegionKey {
    u64 value = 0;

    friend constexpr bool operator==(RegionKey, RegionKey) noexcept = default;
};

/// Where a generated identity comes from. All three are spellable so that the two that lose can be
/// REFUSED by name; see the header comment.
enum class IdentitySource : u8 {
    /// seed, node, region and the candidate's own slot, through `substream` then `draw`. The only
    /// one the compiler accepts.
    Derived = 0,
    /// A global monotonic counter incremented as instances are emitted. Mis-bound 43% of the
    /// spike's overrides on a FULL regeneration.
    TraversalCounter,
    /// (region, ordinal among the region's SURVIVING instances). Reproduces a regeneration and does
    /// not survive an edit: reject one candidate and every survivor after it renumbers.
    SurvivorRank,
};

[[nodiscard]] const char* identity_source_name(IdentitySource source) noexcept;

/// The moment every generation draw is taken at.
///
/// Generation is not ticked: a region regenerated at tick 900 000 must produce exactly what it
/// produced at tick 0, so the simulation point is a constant and every bit of per-instance
/// variation comes from the substream and the sample index instead. `foliage`'s
/// `generation_point()` is the same constant for the same reason, and the two agree by having the
/// same value rather than by one calling the other — this module does not depend on `cy::foliage`.
[[nodiscard]] inline determinism::SimulationPoint generation_point() noexcept {
    return determinism::SimulationPoint{};
}

/// A node's identity from its authored name. A compile-time constant where the name is a literal.
[[nodiscard]] constexpr NodeIdentity node_identity(const char* name) noexcept {
    return NodeIdentity{determinism::stream_id(name).value};
}

/// A region's key from its integer coordinate and level.
///
/// Folded through `substream` rather than by bit-packing, because a packed key is only injective
/// while the coordinate fits the bits allotted to it, and a world generated at region level 2 with
/// negative coordinates is exactly where a packing quietly stops being one.
[[nodiscard]] RegionKey region_key(i32 x, i32 z, u8 level) noexcept;

/// THE DERIVATION. seed, node, region, slot — `simulation-and-determinism`'s stable identifiers,
/// through its own hierarchical mechanism.
///
/// `slot` is the candidate's index in the generating node's OWN sequence, before any rejection.
/// Passing a rank among survivors here would produce `SurvivorRank` under a different name, which
/// is why `PointSet` stores the slot rather than letting a filtered set recompute it.
[[nodiscard]] GeneratedId derive_identity(u64 seed, NodeIdentity node, RegionKey region,
                                          u32 slot) noexcept;

/// The randomness a node draws with: one stream per (node, region), so that a node which begins
/// drawing one more value per candidate shifts nothing any other node or region computes.
///
/// Separate from the identity derivation above — `simulation-and-determinism`: "consuming
/// randomness in one does not shift another's sequence" — so that adding a placement draw cannot
/// renumber a world's instances.
[[nodiscard]] determinism::RandomStream generation_stream(u64 seed, NodeIdentity node,
                                                          RegionKey region) noexcept;

}  // namespace cy::pcg

namespace cy {

template <>
struct Hash<pcg::GeneratedId> {
    [[nodiscard]] u64 operator()(pcg::GeneratedId id) const noexcept {
        return hash_integer(id.value, hash_seed());
    }
};

}  // namespace cy
