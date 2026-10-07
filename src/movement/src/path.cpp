// SPDX-License-Identifier: MIT
// A*, the funnel and path following over a `Fixed` world. Design §9.1.

#include <cy/core/detmath/functions.h>
#include <cy/movement/path.h>
#include <cy/navigation/follow.h>

namespace cy::movement {

namespace {

/// The open list's order: f, then the polygon index. Total, so a pop is the same on every peer.
template <class Entry>
[[nodiscard]] bool before(const Entry& a, const Entry& b) noexcept {
    return a.f < b.f || (a.f == b.f && a.poly < b.poly);
}

}  // namespace

FixedPathSearch::FixedPathSearch(Allocator& allocator) noexcept
    : nodes_(allocator), heap_(allocator) {}

void FixedPathSearch::push(Open entry) noexcept {
    if (!heap_.push_back(entry)) {
        return;
    }
    usize child = heap_.size() - 1;
    while (child > 0) {
        const usize parent = (child - 1) / 2;
        if (!before(heap_[child], heap_[parent])) {
            break;
        }
        const Open swap = heap_[child];
        heap_[child] = heap_[parent];
        heap_[parent] = swap;
        child = parent;
    }
}

FixedPathSearch::Open FixedPathSearch::pop() noexcept {
    const Open top = heap_[0];
    heap_[0] = heap_.back();
    heap_.pop_back();
    usize parent = 0;
    for (;;) {
        const usize left = (2 * parent) + 1;
        const usize right = left + 1;
        usize smallest = parent;
        if (left < heap_.size() && before(heap_[left], heap_[smallest])) {
            smallest = left;
        }
        if (right < heap_.size() && before(heap_[right], heap_[smallest])) {
            smallest = right;
        }
        if (smallest == parent) {
            break;
        }
        const Open swap = heap_[parent];
        heap_[parent] = heap_[smallest];
        heap_[smallest] = swap;
        parent = smallest;
    }
    return top;
}

FixedPathResult FixedPathSearch::find_corridor(const FixedNavMesh& mesh, FixedVec2 start,
                                               FixedVec2 goal, u32 node_budget,
                                               Array<FixedPolyIndex>& corridor) noexcept {
    FixedPathResult result;
    corridor.clear();
    FixedVec2 start_on = start;
    FixedVec2 goal_on = goal;
    const FixedPolyIndex start_poly = mesh.nearest(start, start_on);
    const FixedPolyIndex goal_poly = mesh.nearest(goal, goal_on);
    if (start_poly == kNoPoly || goal_poly == kNoPoly) {
        return result;
    }
    if (nodes_.size() < mesh.poly_count() && !nodes_.resize(mesh.poly_count())) {
        return result;
    }
    ++generation_;
    heap_.clear();

    const auto heuristic = [&](FixedPolyIndex poly) noexcept {
        return detmath::distance(mesh.poly(poly).centre, goal_on);
    };
    nodes_[start_poly] = Node{Fixed::zero(), heuristic(start_poly), kNoPoly, generation_, false};
    push(Open{nodes_[start_poly].f, start_poly});
    FixedPolyIndex closest = start_poly;
    Fixed closest_distance = heuristic(start_poly);

    FixedPolyIndex reached = kNoPoly;
    while (!heap_.empty()) {
        const Open open = pop();
        Node& node = nodes_[open.poly];
        if (node.generation != generation_ || node.closed || open.f != node.f) {
            continue;  // a stale entry: the polygon was reached more cheaply since it was pushed
        }
        node.closed = true;
        ++result.nodes_expanded;
        if (open.poly == goal_poly) {
            reached = goal_poly;
            break;
        }
        if (result.nodes_expanded >= node_budget) {
            result.budget_exceeded = true;
            break;
        }
        const Span<const FixedPolyIndex> across = mesh.neighbours_of(open.poly);
        const Span<const Fixed> lengths = mesh.crossings_of(open.poly);
        for (usize edge = 0; edge < across.size(); ++edge) {
            const FixedPolyIndex next = across[edge];
            if (next == kNoPoly || mesh.blocked(next)) {
                continue;
            }
            const Fixed g = node.g + (lengths[edge] * mesh.poly(next).cost);
            Node& successor = nodes_[next];
            if (successor.generation == generation_ && (successor.closed || !(g < successor.g))) {
                continue;
            }
            const Fixed h = heuristic(next);
            successor = Node{g, g + h, open.poly, generation_, false};
            push(Open{successor.f, next});
            if (h < closest_distance || (h == closest_distance && next < closest)) {
                closest = next;
                closest_distance = h;
            }
        }
    }

    result.found = reached != kNoPoly;
    result.partial = !result.found;
    const FixedPolyIndex end = result.found ? reached : closest;
    result.cost = nodes_[end].g;
    for (FixedPolyIndex poly = end; poly != kNoPoly; poly = nodes_[poly].parent) {
        if (!corridor.push_back(poly)) {
            corridor.clear();
            result.found = false;
            return result;
        }
    }
    for (usize low = 0, high = corridor.size() - 1; low < high; ++low, --high) {
        const FixedPolyIndex swap = corridor[low];
        corridor[low] = corridor[high];
        corridor[high] = swap;
    }
    return result;
}

namespace {

/// One portal, as (left, right) seen walking from one polygon to the next.
struct Portal {
    FixedVec2 left;
    FixedVec2 right;
};

/// The corridor's portals: the start and the goal, both degenerate, around every shared edge.
/// "Left" is the end with the larger exact cross product against the direction of travel.
[[nodiscard]] Status build_portals(const FixedNavMesh& mesh, Span<const FixedPolyIndex> corridor,
                                   FixedVec2 start, FixedVec2 goal,
                                   Array<Portal>& portals) noexcept {
    if (Status pushed = portals.push_back(Portal{start, start}); !pushed) {
        return pushed;
    }
    for (usize k = 0; k + 1 < corridor.size(); ++k) {
        FixedVec2 a;
        FixedVec2 b;
        if (!mesh.portal(corridor[k], corridor[k + 1], a, b)) {
            return fail(ErrorCode::InvalidArgument,
                        "movement: a corridor step crosses no shared edge");
        }
        const FixedVec2 from = mesh.poly(corridor[k]).centre;
        const FixedVec2 travel = mesh.poly(corridor[k + 1]).centre - from;
        const bool a_is_left = detmath::cross(travel, a - from) > detmath::cross(travel, b - from);
        if (Status pushed = portals.push_back(a_is_left ? Portal{a, b} : Portal{b, a}); !pushed) {
            return pushed;
        }
    }
    return portals.push_back(Portal{goal, goal});
}

[[nodiscard]] bool strictly_positive(WideFixed value) noexcept {
    return !value.negative() && value != WideFixed{};
}

/// The simple stupid funnel's state. `left` and `right` bound the funnel from `apex`; a portal end
/// that narrows it moves the bound, and one that crosses the other bound turns that bound into the
/// new apex. Every decision is the sign of an exact cross product.
class Funnel {
public:
    explicit Funnel(FixedVec2 start) noexcept : apex_(start), left_(start), right_(start) {}

    /// Offer portal `i`'s right end. False when it crossed the left bound, which became the apex:
    /// `corner` is then the point to emit, and the walk restarts after the new apex.
    [[nodiscard]] bool narrow_right(FixedVec2 next, usize i, FixedVec2& corner) noexcept {
        if (detmath::cross(right_ - apex_, next - apex_).negative()) {
            return true;  // widens the funnel: ignored
        }
        if (apex_ == right_ || detmath::cross(left_ - apex_, next - apex_).negative()) {
            right_ = next;
            right_index_ = i;
            return true;
        }
        corner = left_;
        restart(left_, left_index_);
        return false;
    }

    /// Offer portal `i`'s left end; the mirror of `narrow_right`.
    [[nodiscard]] bool narrow_left(FixedVec2 next, usize i, FixedVec2& corner) noexcept {
        if (strictly_positive(detmath::cross(left_ - apex_, next - apex_))) {
            return true;  // widens the funnel: ignored
        }
        if (apex_ == left_ || strictly_positive(detmath::cross(right_ - apex_, next - apex_))) {
            left_ = next;
            left_index_ = i;
            return true;
        }
        corner = right_;
        restart(right_, right_index_);
        return false;
    }

    [[nodiscard]] usize apex_index() const noexcept { return apex_index_; }

private:
    void restart(FixedVec2 apex, usize index) noexcept {
        apex_ = apex;
        left_ = apex;
        right_ = apex;
        apex_index_ = index;
        left_index_ = index;
        right_index_ = index;
    }

    FixedVec2 apex_;
    FixedVec2 left_;
    FixedVec2 right_;
    usize apex_index_ = 0;
    usize left_index_ = 0;
    usize right_index_ = 0;
};

}  // namespace

Status straighten(const FixedNavMesh& mesh, Span<const FixedPolyIndex> corridor, FixedVec2 start,
                  FixedVec2 goal, Array<FixedVec2>& out) noexcept {
    out.clear();
    if (Status pushed = out.push_back(start); !pushed) {
        return pushed;
    }
    const auto append = [&out](FixedVec2 point) noexcept -> Status {
        return out.back() == point ? ok() : out.push_back(point);
    };
    Array<Portal> portals(out.allocator());
    if (Status built = build_portals(mesh, corridor, start, goal, portals); !built) {
        return built;
    }
    Funnel funnel(start);
    for (usize i = 1; i < portals.size(); ++i) {
        FixedVec2 corner;
        if (!funnel.narrow_right(portals[i].right, i, corner) ||
            !funnel.narrow_left(portals[i].left, i, corner)) {
            if (Status added = append(corner); !added) {
                return added;
            }
            i = funnel.apex_index();  // the loop's increment resumes after the new apex
        }
    }
    return append(goal);
}

FixedPathResult find_path(FixedPathSearch& search, const FixedNavMesh& mesh, FixedVec2 start,
                          FixedVec2 goal, u32 node_budget, Array<FixedPolyIndex>& corridor,
                          Array<FixedVec2>& out) noexcept {
    out.clear();
    FixedPathResult result = search.find_corridor(mesh, start, goal, node_budget, corridor);
    if (corridor.empty()) {
        result.found = false;
        return result;
    }
    // The ends are snapped onto the polygons the corridor starts and ends in, so the funnel walks
    // from a point it can stand on to one it can.
    const FixedVec2 from = mesh.closest_point(corridor[0], start);
    const FixedVec2 to = mesh.closest_point(corridor[corridor.size() - 1], goal);
    if (!straighten(mesh, corridor.span(), from, to, out)) {
        result.found = false;
        out.clear();
    }
    return result;
}

namespace {

/// `navigation::follow_points`' fixed-point instantiation: arrival decided on the exact squares,
/// and toward the point at the speed that covers the remaining offset in one tick, capped at
/// `speed`.
struct FixedFollowPolicy {
    using Vec = FixedVec2;
    using Scalar = Fixed;

    Fixed tick_rate;

    [[nodiscard]] static FixedVec2 position(FixedVec2 point) noexcept { return point; }
    [[nodiscard]] static FixedVec2 offset(FixedVec2 to, FixedVec2 from) noexcept {
        return to - from;
    }
    [[nodiscard]] static bool beyond(FixedVec2 offset, Fixed arrival) noexcept {
        return WideFixed::product(arrival, arrival) < detmath::length_squared(offset);
    }
    [[nodiscard]] FixedVec2 toward(FixedVec2 offset, Fixed speed) const noexcept {
        return detmath::clamp_length(offset * tick_rate, speed);
    }
};

}  // namespace

FixedVec2 follow_path(Span<const FixedVec2> path, FixedVec2 position, Fixed speed, Fixed arrival,
                      Fixed tick_rate, u32& cursor) noexcept {
    return navigation::follow_points(FixedFollowPolicy{tick_rate}, path, position, speed, arrival,
                                     cursor);
}

}  // namespace cy::movement
