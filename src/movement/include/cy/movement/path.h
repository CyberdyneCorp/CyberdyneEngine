// SPDX-License-Identifier: MIT
#pragma once
// Path queries over a `Fixed` world: A* to a corridor, the funnel to a point path, and following
// it. Design §9.1 and task 6.2.
//
// The same algorithms `navigation` specifies — "A* over the polygon adjacency graph with a
// Euclidean heuristic, producing a corridor of polygons, then a point path" by "the funnel
// algorithm (simple stupid funnel)" — over the converted mesh, with every quantity a `Fixed`:
//
//   g-cost     the distance between successive polygon centres (correctly rounded square root of
//              the exact Q64.64 square) times the entered polygon's cost per metre
//   heuristic  the straight-line distance from a polygon's centre to the goal, the same square root
//   order      (f, polygon index): a total order, so the open list pops the same node on every
//              peer, and two equally short corridors resolve to the same one
//   funnel     the signs of exact Q64.64 cross products, so a collinear or near-collinear portal
//              is decided identically everywhere
//
// A query that cannot reach the goal answers the corridor to the reachable polygon whose centre is
// nearest it, flagged partial — `navigation`'s "the reachable point closest to the target ...
// rather than failing silently". A blocked polygon (`FixedNavMesh::set_blocked`) is never entered.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/movement/nav_mesh.h>

namespace cy::movement {

/// What a search did.
struct FixedPathResult {
    bool found = false;
    bool partial = false;
    bool budget_exceeded = false;
    u32 nodes_expanded = 0;
    /// The g-cost of the corridor's last polygon.
    Fixed cost;
};

/// Reusable search state, so a session that runs many queries a tick allocates once.
class FixedPathSearch {
public:
    explicit FixedPathSearch(Allocator& allocator) noexcept;

    /// A* from `start` to `goal`, both snapped onto the mesh, writing the corridor of polygon
    /// indices into `corridor` (cleared first). Expands at most `node_budget` polygons.
    [[nodiscard]] FixedPathResult find_corridor(const FixedNavMesh& mesh, FixedVec2 start,
                                                FixedVec2 goal, u32 node_budget,
                                                Array<FixedPolyIndex>& corridor) noexcept;

private:
    struct Node {
        Fixed g;
        Fixed f;
        FixedPolyIndex parent = kNoPoly;
        u32 generation = 0;
        bool closed = false;
    };
    struct Open {
        Fixed f;
        FixedPolyIndex poly = kNoPoly;
    };

    void push(Open entry) noexcept;
    [[nodiscard]] Open pop() noexcept;

    Array<Node> nodes_;
    Array<Open> heap_;
    u32 generation_ = 0;
};

/// The funnel: the taut point path from `start` to `goal` through `corridor`, written into `out`
/// (cleared first). The first point is `start`, the last `goal`.
[[nodiscard]] Status straighten(const FixedNavMesh& mesh, Span<const FixedPolyIndex> corridor,
                                FixedVec2 start, FixedVec2 goal, Array<FixedVec2>& out) noexcept;

/// A* and the funnel together: the point path from `start` to `goal`. `goal` is snapped onto the
/// mesh first, so the path ends where a unit can stand.
[[nodiscard]] FixedPathResult find_path(FixedPathSearch& search, const FixedNavMesh& mesh,
                                        FixedVec2 start, FixedVec2 goal, u32 node_budget,
                                        Array<FixedPolyIndex>& corridor,
                                        Array<FixedVec2>& out) noexcept;

/// The velocity that carries a unit at `position` along `path` from point `cursor`, and the cursor
/// it should now hold. A point is reached when the unit is within `arrival` of it (decided on the
/// exact squares). Toward the last point the velocity is the remaining distance times `tick_rate`,
/// capped at `speed`, so a unit arrives without overshooting; toward any other it is `speed`.
/// Zero once the last point is reached.
[[nodiscard]] FixedVec2 follow_path(Span<const FixedVec2> path, FixedVec2 position, Fixed speed,
                                    Fixed arrival, Fixed tick_rate, u32& cursor) noexcept;

}  // namespace cy::movement
