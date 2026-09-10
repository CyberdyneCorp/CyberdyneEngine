// Path queries. See cy/navigation/query.h for the argument.

#include <cy/core/memory/hash_map.h>
#include <cy/navigation/query.h>

#include <algorithm>
#include <cmath>

namespace cy::navigation {
namespace {

/// Twice the signed area of the triangle, in the XZ plane. Positive means `c` is counter-clockwise
/// of the segment `a b` — which this file calls "left", and the funnel's whole correctness is that
/// this convention and `portal_sides()` below agree.
[[nodiscard]] f32 triarea2(Vec3 a, Vec3 b, Vec3 c) noexcept {
    return ((b.x - a.x) * (c.z - a.z)) - ((b.z - a.z) * (c.x - a.x));
}

[[nodiscard]] f32 distance_xz(Vec3 a, Vec3 b) noexcept {
    const f32 dx = b.x - a.x;
    const f32 dz = b.z - a.z;
    return std::sqrt((dx * dx) + (dz * dz));
}

[[nodiscard]] f32 distance3(Vec3 a, Vec3 b) noexcept {
    return length(b - a);
}

/// The portal between two corridor polygons, with its endpoints ordered so that `left` is
/// counter-clockwise of the direction of travel. `NavMesh::portal` answers the edge in the source
/// polygon's winding, which is consistent within a tile and NOT across two tiles cooked separately
/// — so the order is decided here, geometrically, rather than trusted.
[[nodiscard]] bool portal_sides(const NavMesh& mesh, PolyRef from, PolyRef to, Vec3& left,
                                Vec3& right) noexcept {
    Vec3 a;
    Vec3 b;
    if (!mesh.portal(from, to, a, b)) {
        return false;
    }
    const NavPoly* source = mesh.poly(from);
    const NavPoly* target = mesh.poly(to);
    if (source == nullptr || target == nullptr) {
        return false;
    }
    if (triarea2(source->centre, target->centre, a) > 0.0f) {
        left = a;
        right = b;
    } else {
        left = b;
        right = a;
    }
    return true;
}

struct SearchNode {
    PolyRef ref;
    Vec3 position;
    f32 g = 0.0f;
    f32 f = 0.0f;
    u32 parent = 0xFFFFFFFFu;
    LinkId via = kInvalidLink;
};

/// Worst first, so `std::push_heap`'s max-heap is a min-heap on `f`. The tie-break on the reference
/// bits is what makes two runs of one scenario produce the same path — see the header.
struct WorseFirst {
    const Array<SearchNode>* nodes;

    [[nodiscard]] bool operator()(u32 a, u32 b) const noexcept {
        const SearchNode& left = (*nodes)[a];
        const SearchNode& right = (*nodes)[b];
        if (left.f != right.f) {
            return left.f > right.f;
        }
        return left.ref.bits() > right.ref.bits();
    }
};

}  // namespace

Status PathCorridor::push(PolyRef poly, LinkId link) noexcept {
    if (Status pushed = polys_.push_back(poly); !pushed) {
        return pushed;
    }
    if (Status pushed = links_.push_back(link); !pushed) {
        polys_.pop_back();
        return pushed;
    }
    return ok();
}

void PathCorridor::advance_to(usize index) noexcept {
    if (index == 0 || index >= polys_.size()) {
        if (index >= polys_.size()) {
            clear();
        }
        return;
    }
    const usize remaining = polys_.size() - index;
    for (usize position = 0; position < remaining; ++position) {
        polys_[position] = polys_[position + index];
        links_[position] = links_[position + index];
    }
    while (polys_.size() > remaining) {
        polys_.pop_back();
        links_.pop_back();
    }
}

bool PathCorridor::valid_against(const NavMesh& mesh) const noexcept {
    return std::ranges::all_of(polys_.span(),
                               [&mesh](PolyRef ref) { return mesh.poly(ref) != nullptr; });
}

// --- A*
// -------------------------------------------------------------------------------------------

/// One edge out of a corridor node: a shared portal, or an off-mesh link.
///
/// The two are resolved into the same three values — where the edge leads, where it is entered, and
/// what crossing it costs — so the relaxation below sees one kind of edge. The link half is what
/// makes `navigation`'s "capability gating" a property of the search rather than of a filter
/// applied afterwards.
struct Edge {
    PolyRef next = kInvalidPoly;
    Vec3 entry;
    LinkId via = kInvalidLink;
    f32 step = 0.0f;
    bool usable = false;
};

[[nodiscard]] Edge portal_edge(const NavMesh& mesh, PolyRef from, Vec3 position,
                               PolyRef neighbour) noexcept {
    Edge edge;
    if (!neighbour.valid()) {
        return edge;
    }
    Vec3 left;
    Vec3 right;
    if (!portal_sides(mesh, from, neighbour, left, right)) {
        return edge;
    }
    edge.next = neighbour;
    edge.entry = (left + right) * 0.5f;
    edge.step = distance3(position, edge.entry);
    edge.usable = true;
    return edge;
}

[[nodiscard]] Edge link_edge(const NavMesh& mesh, PolyRef from, LinkId via,
                             const PathFilter& filter) noexcept {
    Edge edge;
    const NavLink* link = mesh.link(via);
    if (link == nullptr) {
        return edge;
    }
    if ((link->requires_capabilities & filter.capabilities) != link->requires_capabilities) {
        return edge;
    }
    const bool forward = (link->from_poly == from);
    if (!forward && !(link->bidirectional && link->to_poly == from)) {
        return edge;
    }
    edge.via = via;
    edge.next = forward ? link->to_poly : link->from_poly;
    edge.entry = forward ? link->to : link->from;
    edge.step = link->cost;
    edge.usable = true;
    return edge;
}

/// Everything one search carries between its expansion and its reconstruction.
struct Search {
    Array<SearchNode> nodes;
    Array<u32> open;
    Array<bool> closed;
    HashMap<u64, u32> index_of;
    u32 best_node = 0;
    f32 best_heuristic = math::kInfinity;

    explicit Search(Allocator& allocator) noexcept
        : nodes(allocator), open(allocator), closed(allocator), index_of(allocator) {}
};

/// Relax one edge out of `current`. False only on an allocation failure, which is the one thing a
/// search cannot carry on past.
[[nodiscard]] bool relax(Search& search, const WorseFirst& worse, u32 current, const Edge& edge,
                         f32 tentative, f32 heuristic, PathResult& result) noexcept {
    u32* existing = search.index_of.find(edge.next.bits());
    if (existing != nullptr) {
        if (tentative >= search.nodes[*existing].g) {
            return true;
        }
        SearchNode& node = search.nodes[*existing];
        node.g = tentative;
        node.f = tentative + heuristic;
        node.parent = current;
        node.via = edge.via;
        node.position = edge.entry;
        search.closed[*existing] = false;
        if (!search.open.push_back(*existing)) {
            return false;
        }
        std::ranges::push_heap(search.open.span(), worse);
        if (heuristic < search.best_heuristic) {
            search.best_heuristic = heuristic;
            search.best_node = *existing;
        }
        return true;
    }

    SearchNode node;
    node.ref = edge.next;
    node.position = edge.entry;
    node.g = tentative;
    node.f = tentative + heuristic;
    node.parent = current;
    node.via = edge.via;
    if (!search.nodes.push_back(node) || !search.closed.push_back(false)) {
        return false;
    }
    const auto slot = static_cast<u32>(search.nodes.size() - 1);
    if (!search.index_of.insert(edge.next.bits(), slot) || !search.open.push_back(slot)) {
        return false;
    }
    std::ranges::push_heap(search.open.span(), worse);
    ++result.nodes_visited;
    if (heuristic < search.best_heuristic) {
        search.best_heuristic = heuristic;
        search.best_node = slot;
    }
    return true;
}

/// Expand one node: every portal and every link out of it, relaxed.
[[nodiscard]] bool expand(const NavMesh& mesh, Search& search, const WorseFirst& worse, u32 current,
                          Vec3 end, const PathFilter& filter, PathResult& result) noexcept {
    const PolyRef ref = search.nodes[current].ref;
    const Vec3 position = search.nodes[current].position;
    const Span<const PolyRef> neighbours = mesh.poly_neighbours(ref);
    const Span<const LinkId> links = mesh.links_from(ref);

    const usize edge_count = neighbours.size() + links.size();
    for (usize index = 0; index < edge_count; ++index) {
        const Edge edge = (index < neighbours.size())
                              ? portal_edge(mesh, ref, position, neighbours[index])
                              : link_edge(mesh, ref, links[index - neighbours.size()], filter);
        if (!edge.usable) {
            continue;
        }
        const NavPoly* target = mesh.poly(edge.next);
        if (target == nullptr) {
            continue;
        }
        const AreaType area = mesh.effective_area(edge.next);
        if ((area_bit(area) & filter.areas) == 0) {
            continue;
        }
        const f32 multiplier = filter.costs.of(area) * target->cost;
        const f32 tentative = search.nodes[current].g + (edge.step * multiplier);
        if (!relax(search, worse, current, edge, tentative, distance3(edge.entry, end), result)) {
            return false;
        }
    }
    return true;
}

PathResult find_corridor(const NavMesh& mesh, PolyRef start_poly, Vec3 start, PolyRef end_poly,
                         Vec3 end, const PathFilter& filter, PathCorridor& corridor) noexcept {
    PathResult result;
    corridor.clear();
    if (mesh.poly(start_poly) == nullptr || mesh.poly(end_poly) == nullptr) {
        return result;
    }

    Search search(mesh.allocator());
    SearchNode first;
    first.ref = start_poly;
    first.position = start;
    first.g = 0.0f;
    first.f = distance3(start, end);
    if (!search.nodes.push_back(first) || !search.closed.push_back(false) ||
        !search.index_of.insert(start_poly.bits(), 0) || !search.open.push_back(0)) {
        return result;
    }
    search.best_heuristic = first.f;

    const WorseFirst worse{&search.nodes};
    bool reached = false;
    u32 goal_node = 0;

    while (!search.open.empty()) {
        if (result.nodes_expanded >= filter.node_budget) {
            result.budget_exceeded = true;
            break;
        }
        std::ranges::pop_heap(search.open.span(), worse);
        const u32 current = search.open[search.open.size() - 1];
        search.open.pop_back();
        if (search.closed[current]) {
            continue;
        }
        search.closed[current] = true;
        ++result.nodes_expanded;

        if (search.nodes[current].ref == end_poly) {
            reached = true;
            goal_node = current;
            break;
        }
        if (!expand(mesh, search, worse, current, end, filter, result)) {
            return result;
        }
    }

    // `navigation`: an unreachable target and an exhausted budget both answer the reachable point
    // closest to the target, flagged as partial. They are the same reconstruction from a different
    // node, and answering nothing in either case is what "fails silently" means.
    const u32 tail = reached ? goal_node : search.best_node;
    result.found = true;
    result.partial = !reached;
    result.cost = search.nodes[tail].g;

    u32 walk = tail;
    u32 depth = 0;
    while (walk != 0xFFFFFFFFu && depth <= search.nodes.size()) {
        if (!corridor.push(search.nodes[walk].ref, search.nodes[walk].via)) {
            result.found = false;
            return result;
        }

        walk = search.nodes[walk].parent;
        ++depth;
    }
    // Reversed: reconstruction walks parents, and every consumer reads a corridor start to end.
    Array<PolyRef> reversed_polys(mesh.allocator());
    Array<LinkId> reversed_links(mesh.allocator());
    const Span<const PolyRef> polys = corridor.polys();
    const Span<const LinkId> links = corridor.links();
    for (usize index = polys.size(); index > 0; --index) {
        if (!reversed_polys.push_back(polys[index - 1]) ||
            !reversed_links.push_back(links[index - 1])) {
            result.found = false;
            return result;
        }
    }
    corridor.clear();
    for (usize index = 0; index < reversed_polys.size(); ++index) {
        if (!corridor.push(reversed_polys[index], reversed_links[index])) {
            result.found = false;
            return result;
        }
    }
    return result;
}

PathResult find_path(const NavMesh& mesh, Vec3 start, Vec3 end, Vec3 extents,
                     const PathFilter& filter, PathCorridor& corridor) noexcept {
    Vec3 start_on;
    Vec3 end_on;
    const PolyRef start_poly = mesh.find_nearest(start, extents, filter.areas, start_on);
    const PolyRef end_poly = mesh.find_nearest(end, extents, filter.areas, end_on);
    return find_corridor(mesh, start_poly, start_on, end_poly, end_on, filter, corridor);
}

bool reachable(const NavMesh& mesh, PolyRef start_poly, Vec3 start, PolyRef end_poly, Vec3 end,
               const PathFilter& filter, f32& cost) noexcept {
    PathCorridor corridor(mesh.allocator());
    const PathResult result =
        find_corridor(mesh, start_poly, start, end_poly, end, filter, corridor);
    cost = result.cost;
    return result.found && !result.partial;
}

// --- The funnel ---------------------------------------------------------------------------------

namespace {

/// The simple stupid funnel over one run of adjacent polygons. `first` and `last` bracket the run
/// within the corridor; `start` and `end` are the run's own endpoints.
[[nodiscard]] Status funnel_segment(const NavMesh& mesh, const PathCorridor& corridor, usize first,
                                    usize last, Vec3 start, Vec3 end,
                                    Array<PathPoint>& out) noexcept {
    const Span<const PolyRef> polys = corridor.polys();

    Array<Vec3> left_side(mesh.allocator());
    Array<Vec3> right_side(mesh.allocator());
    Array<PolyRef> owner(mesh.allocator());
    if (!left_side.push_back(start) || !right_side.push_back(start) ||
        !owner.push_back(polys[first])) {
        return fail(ErrorCode::OutOfMemory, "the funnel could not hold its portals");
    }
    for (usize index = first; index + 1 <= last; ++index) {
        Vec3 left;
        Vec3 right;
        if (!portal_sides(mesh, polys[index], polys[index + 1], left, right)) {
            return fail(ErrorCode::InvalidArgument,
                        "the corridor contains two polygons that do not share an edge");
        }
        if (!left_side.push_back(left) || !right_side.push_back(right) ||
            !owner.push_back(polys[index + 1])) {
            return fail(ErrorCode::OutOfMemory, "the funnel could not hold its portals");
        }
    }
    if (!left_side.push_back(end) || !right_side.push_back(end) || !owner.push_back(polys[last])) {
        return fail(ErrorCode::OutOfMemory, "the funnel could not hold its portals");
    }

    Vec3 apex = start;
    Vec3 left = start;
    Vec3 right = start;
    usize apex_index = 0;
    usize left_index = 0;
    usize right_index = 0;

    if (out.empty()) {
        PathPoint point;
        point.position = start;
        point.poly = polys[first];
        if (Status pushed = out.push_back(point); !pushed) {
            return pushed;
        }
    }

    for (usize index = 1; index < left_side.size(); ++index) {
        const Vec3 candidate_left = left_side[index];
        const Vec3 candidate_right = right_side[index];

        // Tighten the right side, unless doing so would cross the left one — in which case the left
        // vertex is a corner of the taut path and the scan restarts from it.
        if (triarea2(apex, right, candidate_right) <= 0.0f) {
            if (distance_xz(apex, right) < 1e-6f || triarea2(apex, left, candidate_right) > 0.0f) {
                right = candidate_right;
                right_index = index;
            } else {
                PathPoint point;
                point.position = left;
                point.poly = owner[left_index];
                if (Status pushed = out.push_back(point); !pushed) {
                    return pushed;
                }
                apex = left;
                apex_index = left_index;
                right = apex;
                left = apex;
                right_index = apex_index;
                left_index = apex_index;
                index = apex_index;
                continue;
            }
        }

        if (triarea2(apex, left, candidate_left) >= 0.0f) {
            if (distance_xz(apex, left) < 1e-6f || triarea2(apex, right, candidate_left) < 0.0f) {
                left = candidate_left;
                left_index = index;
            } else {
                PathPoint point;
                point.position = right;
                point.poly = owner[right_index];
                if (Status pushed = out.push_back(point); !pushed) {
                    return pushed;
                }
                apex = right;
                apex_index = right_index;
                right = apex;
                left = apex;
                right_index = apex_index;
                left_index = apex_index;
                index = apex_index;
                continue;
            }
        }
    }

    PathPoint point;
    point.position = end;
    point.poly = polys[last];
    return out.push_back(point);
}

}  // namespace

Status straighten(const NavMesh& mesh, const PathCorridor& corridor, Vec3 start, Vec3 end,
                  Array<PathPoint>& out) noexcept {
    out.clear();
    const Span<const PolyRef> polys = corridor.polys();
    const Span<const LinkId> links = corridor.links();
    if (polys.empty()) {
        return ok();
    }
    if (polys.size() == 1) {
        PathPoint first;
        first.position = start;
        first.poly = polys[0];
        if (Status pushed = out.push_back(first); !pushed) {
            return pushed;
        }
        PathPoint last;
        last.position = end;
        last.poly = polys[0];
        return out.push_back(last);
    }

    // An off-mesh link is not a shared edge, so the funnel cannot see through it: the corridor is
    // funnelled in runs between links, and the link's own two endpoints are inserted between them,
    // tagged with its action. That is `navigation`'s "the path SHALL include the link's endpoints
    // tagged with its action, and the agent's gameplay code SHALL perform the jump".
    usize run_start = 0;
    Vec3 run_from = start;
    for (usize index = 1; index <= polys.size(); ++index) {
        const bool at_end = (index == polys.size());
        const LinkId via = at_end ? kInvalidLink : links[index];
        if (!at_end && via == kInvalidLink) {
            continue;
        }
        const NavLink* link = at_end ? nullptr : mesh.link(via);
        const Vec3 run_to = (link != nullptr) ? link->from : end;
        if (Status done =
                funnel_segment(mesh, corridor, run_start, index - 1, run_from, run_to, out);
            !done) {
            return done;
        }
        if (link == nullptr) {
            break;
        }
        // The point that enters the link, and the one that leaves it.
        if (!out.empty()) {
            out[out.size() - 1].link = via;
            out[out.size() - 1].action = link->action;
            out[out.size() - 1].enters_link = true;
        }
        PathPoint exit;
        exit.position = link->to;
        exit.poly = polys[index];
        exit.link = via;
        exit.action = link->action;
        if (Status pushed = out.push_back(exit); !pushed) {
            return pushed;
        }
        run_start = index;
        run_from = link->to;
    }
    return ok();
}

void simplify(Array<PathPoint>& path, f32 tolerance) noexcept {
    if (path.size() < 3 || tolerance <= 0.0f) {
        return;
    }
    usize write = 1;
    for (usize index = 1; index + 1 < path.size(); ++index) {
        // A point that brackets an off-mesh link is never dropped: gameplay reads the action from
        // it, and a simplification pass that removed it would remove the jump.
        if (path[index].link != kInvalidLink) {
            path[write++] = path[index];
            continue;
        }
        const Vec3 previous = path[write - 1].position;
        const Vec3 next = path[index + 1].position;
        const Vec3 segment = next - previous;
        const f32 length_sq = dot(segment, segment);
        f32 distance = 0.0f;
        if (length_sq <= 1e-8f) {
            distance = distance3(path[index].position, previous);
        } else {
            const f32 t = dot(path[index].position - previous, segment) / length_sq;
            const f32 clamped = std::fmin(std::fmax(t, 0.0f), 1.0f);
            distance = distance3(path[index].position, previous + (segment * clamped));
        }
        if (distance > tolerance) {
            path[write++] = path[index];
        }
    }
    path[write++] = path[path.size() - 1];
    while (path.size() > write) {
        path.pop_back();
    }
}

Status round_corners(Array<PathPoint>& path, f32 radius, u32 segments) noexcept {
    if (path.size() < 3 || radius <= 0.0f || segments == 0) {
        return ok();
    }
    Array<PathPoint> rounded(path.allocator());
    if (Status reserved = rounded.reserve(path.size() + (path.size() * segments)); !reserved) {
        return reserved;
    }
    if (Status pushed = rounded.push_back(path[0]); !pushed) {
        return pushed;
    }
    for (usize index = 1; index + 1 < path.size(); ++index) {
        const Vec3 corner = path[index].position;
        const Vec3 into = corner - path[index - 1].position;
        const Vec3 out_of = path[index + 1].position - corner;
        const f32 into_length = length(into);
        const f32 out_length = length(out_of);
        if (into_length < 1e-4f || out_length < 1e-4f || path[index].link != kInvalidLink) {
            if (Status pushed = rounded.push_back(path[index]); !pushed) {
                return pushed;
            }
            continue;
        }
        const f32 cut = std::min(radius, std::min(into_length, out_length) * 0.5f);
        const Vec3 entry = corner - (into * (cut / into_length));
        const Vec3 exit = corner + (out_of * (cut / out_length));
        // A quadratic Bezier with the corner as its control point: it starts and ends tangent to
        // the two segments, which is what makes the result followable by a vehicle.
        for (u32 step = 0; step <= segments; ++step) {
            const f32 t = static_cast<f32>(step) / static_cast<f32>(segments);
            const f32 inverse = 1.0f - t;
            PathPoint point = path[index];
            point.position =
                (entry * (inverse * inverse)) + (corner * (2.0f * inverse * t)) + (exit * (t * t));
            if (Status pushed = rounded.push_back(point); !pushed) {
                return pushed;
            }
        }
    }
    if (Status pushed = rounded.push_back(path[path.size() - 1]); !pushed) {
        return pushed;
    }
    path.clear();
    return path.append(rounded.span());
}

// --- The deterministic queue
// ----------------------------------------------------------------------

PathQueue::PathQueue(Allocator& allocator, const NavMesh& mesh, u32 latency_ticks) noexcept
    : mesh_(&mesh),
      entries_(allocator),
      completed_(allocator),
      latency_((latency_ticks == 0) ? 1 : latency_ticks) {}

Expected<QueryId, Error> PathQueue::submit(u64 owner, Vec3 start, Vec3 end, Vec3 extents,
                                           const PathFilter& filter, u32 tick) noexcept {
    Expected<Entry*, Error> entry = entries_.emplace_back(entries_.allocator());
    if (!entry) {
        return make_unexpected(entry.error());
    }
    (*entry)->owner = owner;
    (*entry)->start = start;
    (*entry)->end = end;
    (*entry)->extents = extents;
    (*entry)->filter = filter;
    (*entry)->deliver_tick = tick + latency_;
    (*entry)->state = QueryState::Pending;
    return static_cast<QueryId>(entries_.size() - 1);
}

Status PathQueue::cancel(QueryId id) noexcept {
    if (id >= entries_.size()) {
        return fail(ErrorCode::NotFound, "no such path query");
    }
    if (entries_[id].state == QueryState::Pending) {
        entries_[id].state = QueryState::Cancelled;
    }
    return ok();
}

u32 PathQueue::update(u32 tick) noexcept {
    completed_.clear();
    u32 count = 0;
    // Submission order, which is the array's order. Whatever a search costs, the tick it lands on
    // and the order it lands in are the same in every run.
    for (usize index = 0; index < entries_.size(); ++index) {
        Entry& entry = entries_[index];
        if (entry.state != QueryState::Pending || entry.deliver_tick > tick) {
            continue;
        }
        entry.result =
            find_path(*mesh_, entry.start, entry.end, entry.extents, entry.filter, entry.corridor);
        entry.state = QueryState::Ready;
        if (!completed_.push_back(static_cast<QueryId>(index))) {
            return count;
        }
        ++count;
    }
    return count;
}

bool PathQueue::consume(QueryId id, PathCorridor& corridor, PathResult& result) noexcept {
    if (id >= entries_.size() || entries_[id].state != QueryState::Ready) {
        return false;
    }
    corridor = std::move(entries_[id].corridor);
    result = entries_[id].result;
    entries_[id].state = QueryState::Consumed;
    return true;
}

QueryState PathQueue::state(QueryId id) const noexcept {
    return (id < entries_.size()) ? entries_[id].state : QueryState::Cancelled;
}

u64 PathQueue::owner(QueryId id) const noexcept {
    return (id < entries_.size()) ? entries_[id].owner : 0;
}

u32 PathQueue::pending() const noexcept {
    u32 count = 0;
    for (const Entry& entry : entries_.span()) {
        count += (entry.state == QueryState::Pending) ? 1u : 0u;
    }
    return count;
}

}  // namespace cy::navigation
