#pragma once
// Quadric-error edge collapse over one group, with an exact locked-vertex set. M7 task 7.1.
//
// This is the "bounded algorithm" `virtual-geometry` says MAY be integrated from a library. It is
// engine code, and the reason is the lock rather than the arithmetic: the crack-free requirement is
// that a group's boundary vertices are *not moved and not removed*, and a simplifier that merely
// weights boundaries heavily satisfies that almost always. Almost always is a hole per frame in a
// large scene. The interface below is the one a library would be dropped into — triangles and a
// locked set in, triangles and an error out — so the decision is reversible.
//
// --- THE ERROR IT REPORTS ------------------------------------------------------------------------
//
// `virtual-geometry`: "Every hierarchy node SHALL carry a **geometric error**: a bound on how far
// its representation deviates from the source geometry." The quadric at a vertex is the sum of
// squared distances to the planes of its original faces, so the quadric evaluated at the collapsed
// position is a squared distance in world units. The reported error is the square root of the
// largest one accepted, which is a distance and composes with the parent's the way the hierarchy
// needs — and it is a bound on THIS level's deviation only, which is why build.cpp takes the max
// with the children's before writing it into a group.
//
// --- WHY IT IS DETERMINISTIC ---------------------------------------------------------------------
//
// A priority queue keyed on a float ties on equal costs, and a symmetric mesh produces equal costs
// constantly. Collapses are therefore selected in ascending (cost, lower vertex, upper vertex)
// order from a list that is rebuilt each pass, and a pass applies every collapse whose endpoints
// have not already been touched. Slower than a heap and byte-identical between runs, which is what
// task 7.5 needs.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>

namespace cy::rendering::vg {

/// One group's triangles, in a shared vertex index space, plus what may not move.
struct SimplifyInput {
    /// Positions in the shared (welded) index space. Indexed by the values in `indices`; entries
    /// the group does not use are ignored.
    Span<const Vec3> positions;
    /// Triangles, three indices each.
    Span<const u32> indices;
    /// Sorted, unique. Every vertex on the group's external boundary, and every vertex on an open
    /// edge of the source mesh. A locked vertex is never moved and never removed.
    Span<const u32> locked;
    /// Stop when this many triangles remain, or when no further collapse is legal.
    u32 target_triangles = 0;
};

struct SimplifyResult {
    explicit SimplifyResult(Allocator& allocator) noexcept : indices(allocator) {}

    SimplifyResult(const SimplifyResult&) = delete;
    SimplifyResult& operator=(const SimplifyResult&) = delete;
    SimplifyResult(SimplifyResult&&) noexcept = default;
    SimplifyResult& operator=(SimplifyResult&&) noexcept = default;

    /// The surviving triangles, in the same shared index space as the input. Vertices that survived
    /// keep their index; nothing is renumbered here, because the caller needs to compare boundary
    /// edges before and after and a renumbering would make that comparison meaningless.
    Array<u32> indices;
    /// World-space distance bound for this simplification alone.
    f32 error = 0.0F;
    /// How many collapses were applied, and how many were refused because an endpoint was locked.
    u32 collapses = 0;
    u32 refused_locked = 0;
};

/// Simplify one group. Never moves or removes a locked vertex, never produces a non-manifold fan
/// flip, and never collapses an edge whose collapse would invert a surviving triangle's normal.
[[nodiscard]] Expected<SimplifyResult, Error> simplify_group(
    const SimplifyInput& input, Allocator& allocator = current_allocator()) noexcept;

}  // namespace cy::rendering::vg
