#pragma once
// The coherence invariants, as a function that returns findings. Task 3.1.10, design.md §3.
//
// `scene-graph-and-nodes` — "Coherence invariants": the engine "SHALL maintain these invariants,
// checked by assertions in development builds":
//
//   1. Every node's entity is alive.
//   2. Every entity with a node has exactly one node.
//   3. A node's ECS `Parent` matches its tree parent.
//   4. `WorldTransform` is consistent with `LocalTransform` and the parent chain after propagation.
//   5. Effective visibility and enablement are consistent with ancestors after propagation.
//
// WHY THIS RETURNS A REPORT RATHER THAN ASSERTING. `CY_ASSERT` is compiled out in the Profile and
// Shipping configurations, so a check written only as an assertion is a check that does not exist
// in half the builds the engine ships — and a *test* written against an assertion passes in those
// two configurations by testing nothing. The check therefore computes a report in every
// configuration and returns it; `CY_ASSERT_COHERENT` is the development-build wrapper the
// specification's wording asks for, and it asserts on the report this function produced.
//
// TWO OF THE FIVE ARE TRUE BY CONSTRUCTION, AND THE CHECK SAYS SO RATHER THAN SKIPPING THEM.
// Invariant 1 cannot fail through the node API because a `Node` stores no liveness — `valid()` asks
// the entity table. Invariant 2 cannot fail because a node *is* its entity: `NodeName` is the node,
// so "exactly one" is "the component is present at most once", which the ECS guarantees. They are
// still checked, because the interesting failures are the ones that arrive from outside the API —
// a system writing `Parent` directly, a snapshot restored into a world whose registry disagrees —
// and those are exactly what the specification's own scenario describes.
//
// COST. Invariants 3 and 5 are O(nodes); invariant 4 recomputes every world transform and compares,
// which is O(nodes) with a `Transform` multiply each. This is a development-build check and a test
// helper, not something a frame runs.

#include <cy/core/base/assert.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/scene/tree.h>

namespace cy::scene {

/// Which invariant a finding is about. The numbering is the specification's.
enum class Invariant : u8 {
    EntityAlive = 1,
    OneNodePerEntity = 2,
    ParentMatchesTree = 3,
    WorldTransformConsistent = 4,
    EffectiveFlagsConsistent = 5,
};

const char* invariant_name(Invariant invariant) noexcept;

/// One violation: which invariant, which entity, and a literal saying what was wrong.
struct CoherenceViolation {
    Invariant invariant = Invariant::EntityAlive;
    Entity entity;
    const char* detail = "";
};

/// What a check found.
struct CoherenceReport {
    u32 nodes_checked = 0;
    u32 violations = 0;
    /// The first `kMaxRecorded` violations. A tree that has gone wrong usually goes wrong for every
    /// node below one edge, and a report holding all of them would be a memory problem on top of a
    /// correctness one.
    static constexpr u32 kMaxRecorded = 16;
    CoherenceViolation recorded[kMaxRecorded];
    u32 recorded_count = 0;

    [[nodiscard]] bool coherent() const noexcept { return violations == 0; }
};

/// Check every invariant over every node in the tree.
///
/// Call it after a propagation: invariants 4 and 5 are stated "after propagation", and checking
/// them mid-frame would report the dirty state propagation exists to resolve.
[[nodiscard]] Expected<CoherenceReport, Error> check_coherence(SceneTree& tree) noexcept;

/// The tolerance invariant 4 compares world transforms with. A recomputed transform is a different
/// sequence of multiplies from the propagated one, so exact equality would report float noise as a
/// violation; this is loose enough for a deep chain and far tighter than any real inconsistency.
inline constexpr f32 kCoherenceTolerance = 1.0e-4F;

/// `check_coherence()` reduced to a bool, for the assertion below. A failed check *and* a failed
/// allocation both answer false: a check that could not run has not established anything.
[[nodiscard]] bool coherent(SceneTree& tree) noexcept;

}  // namespace cy::scene

/// The development-build assertion the specification asks for.
///
/// The call is inside the assertion's expression rather than beside it, so that in Profile and
/// Shipping — where `CY_ASSERT_MSG` leaves its expression unevaluated — the O(nodes) walk is not
/// merely unchecked but never runs. A check that cost a frame in a shipping build to discard its
/// answer would be worse than no check at all.
#define CY_ASSERT_COHERENT(tree) \
    CY_ASSERT_MSG(::cy::scene::coherent(tree), "the scene tree violates a coherence invariant")
