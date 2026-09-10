#pragma once
// Path queries: A* over the polygon graph, the funnel that turns a corridor into a point path, and
// the deterministic asynchronous queue those queries are issued through. M8.b task 6.1.
//
// ================================================================================================
// WHAT `navigation` FIXES HERE, AND WHAT IT LEAVES OPEN
// ================================================================================================
//
// Fixed by the specification, so not a choice this file makes:
//
//   * "Path queries SHALL run A* over the polygon adjacency graph with a Euclidean heuristic,
//     producing a CORRIDOR of polygons, THEN a point path."
//   * "Corridors SHALL be converted to point paths by the FUNNEL ALGORITHM (simple stupid funnel),
//     producing a taut string-pulled path."
//   * Every query carries area cost multipliers, an area mask, a NODE BUDGET, and answers a PARTIAL
//     path rather than failing silently.
//   * "Asynchronous queries SHALL complete DETERMINISTICALLY: a query issued on a given tick SHALL
//     deliver its result on a defined later tick and be applied in a deterministic order."
//
// The last is the one that shapes the interface. A queue that completed a query when its job
// finished would make an agent's decision depend on how busy the machine was, and `ai-system`
// requires AI decisions that survive replay. So `PathQueue` delivers on `submit_tick + latency`,
// in submission order, whatever the work actually cost — and `PathQueue::update` is where the work
// happens, so a caller can run it on a worker without changing when anything is applied.
//
// TIE-BREAKING IS PART OF THE CONTRACT. Two polygons that reach the goal at the same cost are
// ordered by their reference bits, which are (tile, salt, polygon) — stable for a given mesh and
// independent of insertion order, allocation and iteration. Without it two runs of one scenario
// produce two different, equally short paths.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/navigation/navmesh.h>

namespace cy::navigation {

/// What one agent may traverse and what it prefers. `navigation`'s "area cost multipliers per agent
/// ... an area mask excluding types entirely ... a maximum search node budget".
struct PathFilter {
    NavAreaCosts costs = NavAreaCosts::uniform();
    AreaMask areas = kAllAreas;
    /// Off-mesh links requiring a capability this does not have are not expanded.
    CapabilityMask capabilities = kAllCapabilities;
    /// The most polygons A* will expand before answering with its best partial result.
    u32 node_budget = 2048;
};

/// The polygon sequence a search produced. `links[i]` is the off-mesh link traversed to ENTER
/// `polys[i]`, or `kInvalidLink` when the step crossed a shared edge.
class PathCorridor {
public:
    explicit PathCorridor(Allocator& allocator) noexcept : polys_(allocator), links_(allocator) {}

    PathCorridor(const PathCorridor&) = delete;
    PathCorridor& operator=(const PathCorridor&) = delete;
    PathCorridor(PathCorridor&&) noexcept = default;
    PathCorridor& operator=(PathCorridor&&) noexcept = default;

    [[nodiscard]] Span<const PolyRef> polys() const noexcept { return polys_.span(); }
    [[nodiscard]] Span<const LinkId> links() const noexcept { return links_.span(); }
    [[nodiscard]] usize size() const noexcept { return polys_.size(); }
    [[nodiscard]] bool empty() const noexcept { return polys_.empty(); }
    void clear() noexcept {
        polys_.clear();
        links_.clear();
    }
    [[nodiscard]] Status push(PolyRef poly, LinkId link) noexcept;

    /// Drop everything before `index`. What an agent does as it advances, so the corridor stays the
    /// remainder of the path rather than the whole of it.
    void advance_to(usize index) noexcept;

    /// Is every polygon still resolvable against `mesh`? False after a tile the corridor crosses
    /// was rebuilt or unloaded — which is `navigation`'s "the agent's path SHALL be invalidated
    /// cleanly".
    [[nodiscard]] bool valid_against(const NavMesh& mesh) const noexcept;

private:
    Array<PolyRef> polys_;
    Array<LinkId> links_;
};

/// What a search did. Every field is a measurement; nothing here is a bare success flag, because
/// `navigation` requires a budget overrun and a partial result to be REPORTED rather than inferred.
struct PathResult {
    bool found = false;
    /// True when the corridor ends at the reachable polygon closest to the target rather than at
    /// the target's own. `navigation`: "return the reachable point closest to the target, flagged
    /// as partial, rather than failing silently".
    bool partial = false;
    bool budget_exceeded = false;
    u32 nodes_expanded = 0;
    u32 nodes_visited = 0;
    f32 cost = 0.0f;
};

/// One point of a straightened path. `link` and `action` are set on the two points that bracket an
/// off-mesh traversal, which is how `navigation`'s "the path SHALL include the link's endpoints
/// tagged with its action" reaches gameplay.
struct PathPoint {
    Vec3 position;
    PolyRef poly;
    LinkId link = kInvalidLink;
    Name action;
    /// True on the first of a link's two points, so a follower knows the traversal starts here.
    bool enters_link = false;
};

// --- Synchronous queries -------------------------------------------------------------------------

/// A* from `start` to `end`, both snapped to the mesh within `extents`.
[[nodiscard]] PathResult find_path(const NavMesh& mesh, Vec3 start, Vec3 end, Vec3 extents,
                                   const PathFilter& filter, PathCorridor& corridor) noexcept;

/// A* between two polygons already resolved. The form the crowd and the hierarchy call.
[[nodiscard]] PathResult find_corridor(const NavMesh& mesh, PolyRef start_poly, Vec3 start,
                                       PolyRef end_poly, Vec3 end, const PathFilter& filter,
                                       PathCorridor& corridor) noexcept;

/// Can `end` be reached from `start` at all, and at what cost? `ai-system`'s environment queries
/// ask this per candidate point, so it answers without producing a corridor.
[[nodiscard]] bool reachable(const NavMesh& mesh, PolyRef start_poly, Vec3 start, PolyRef end_poly,
                             Vec3 end, const PathFilter& filter, f32& cost) noexcept;

/// The funnel. Turns a corridor into a taut point path, breaking at every off-mesh link.
[[nodiscard]] Status straighten(const NavMesh& mesh, const PathCorridor& corridor, Vec3 start,
                                Vec3 end, Array<PathPoint>& out) noexcept;

/// Drop points that lie within `tolerance` of the segment between their neighbours.
void simplify(Array<PathPoint>& path, f32 tolerance) noexcept;

/// Round corners to `radius`, inserting two points per corner. `navigation`: "a corner-radius
/// smoothing pass producing curved paths for vehicles" — followable by a non-holonomic agent.
[[nodiscard]] Status round_corners(Array<PathPoint>& path, f32 radius, u32 segments) noexcept;

// --- Asynchronous queries ------------------------------------------------------------------------

using QueryId = u32;
inline constexpr QueryId kInvalidQuery = 0xFFFFFFFFu;

enum class QueryState : u8 { Pending = 0, Ready, Consumed, Cancelled };

/// A queue whose completion tick is a function of the tick a query was submitted on, and of nothing
/// else. See the header comment: this is `navigation`'s "deterministic async delivery" scenario and
/// `ai-system`'s "async completion is deterministic", and it is why the class exists at all rather
/// than callers calling `find_path` from a job.
class PathQueue {
public:
    PathQueue(Allocator& allocator, const NavMesh& mesh, u32 latency_ticks) noexcept;

    PathQueue(const PathQueue&) = delete;
    PathQueue& operator=(const PathQueue&) = delete;

    [[nodiscard]] u32 latency() const noexcept { return latency_; }

    /// Enqueue. `owner` is the caller's own identifier, returned with the result so a batch of
    /// completions can be applied without a second table.
    [[nodiscard]] Expected<QueryId, Error> submit(u64 owner, Vec3 start, Vec3 end, Vec3 extents,
                                                  const PathFilter& filter, u32 tick) noexcept;
    [[nodiscard]] Status cancel(QueryId id) noexcept;

    /// Run the searches whose delivery tick has arrived, in submission order. Returns how many
    /// completed.
    [[nodiscard]] u32 update(u32 tick) noexcept;

    /// Take one completed result. The corridor is moved out, so a caller holds it rather than the
    /// queue.
    [[nodiscard]] bool consume(QueryId id, PathCorridor& corridor, PathResult& result) noexcept;

    /// The queries that completed on the last `update`, in the order they were submitted.
    [[nodiscard]] Span<const QueryId> completed() const noexcept { return completed_.span(); }

    [[nodiscard]] QueryState state(QueryId id) const noexcept;
    [[nodiscard]] u64 owner(QueryId id) const noexcept;
    [[nodiscard]] u32 pending() const noexcept;

private:
    struct Entry {
        u64 owner = 0;
        Vec3 start;
        Vec3 end;
        Vec3 extents;
        PathFilter filter;
        PathCorridor corridor;
        PathResult result;
        u32 deliver_tick = 0;
        QueryState state = QueryState::Pending;

        explicit Entry(Allocator& allocator) noexcept : corridor(allocator) {}
    };

    const NavMesh* mesh_;
    Array<Entry> entries_;
    Array<QueryId> completed_;
    u32 latency_ = 1;
};

}  // namespace cy::navigation
