// SPDX-License-Identifier: MIT
// A*, the funnel and path following over a `Fixed` world. Design §9.1.

#include <cy/core/detmath/functions.h>
#include <cy/movement/path.h>

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

Status straighten(const FixedNavMesh& mesh, Span<const FixedPolyIndex> corridor, FixedVec2 start,
                  FixedVec2 goal, Array<FixedVec2>& out) noexcept {
    out.clear();
    if (Status pushed = out.push_back(start); !pushed) {
        return pushed;
    }
    const auto append = [&out](FixedVec2 point) noexcept -> Status {
        return out.back() == point ? ok() : out.push_back(point);
    };

    // The portals, each as (left, right) seen walking from one polygon to the next: "left" is the
    // end with the larger exact cross product against the direction of travel. The first portal is
    // the start and the last the goal, both degenerate.
    struct Portal {
        FixedVec2 left;
        FixedVec2 right;
    };
    Array<Portal> portals(out.allocator());
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
    if (Status pushed = portals.push_back(Portal{goal, goal}); !pushed) {
        return pushed;
    }

    // The simple stupid funnel. `left` and `right` bound the funnel from `apex`; a portal end that
    // narrows it moves the bound, and one that crosses the other bound turns that bound into the
    // new apex. Every decision is the sign of an exact cross product.
    FixedVec2 apex = start;
    FixedVec2 left = start;
    FixedVec2 right = start;
    usize apex_index = 0;
    usize left_index = 0;
    usize right_index = 0;
    for (usize i = 1; i < portals.size(); ++i) {
        const FixedVec2 next_left = portals[i].left;
        const FixedVec2 next_right = portals[i].right;

        if (!detmath::cross(right - apex, next_right - apex).negative()) {
            if (apex == right || detmath::cross(left - apex, next_right - apex).negative()) {
                right = next_right;
                right_index = i;
            } else {
                if (Status added = append(left); !added) {
                    return added;
                }
                apex = left;
                apex_index = left_index;
                right = apex;
                right_index = apex_index;
                i = apex_index;
                continue;
            }
        }

        const WideFixed narrowing = detmath::cross(left - apex, next_left - apex);
        if (narrowing.negative() || narrowing == WideFixed{}) {
            const WideFixed beyond = detmath::cross(right - apex, next_left - apex);
            if (apex == left || (!beyond.negative() && beyond != WideFixed{})) {
                left = next_left;
                left_index = i;
            } else {
                if (Status added = append(right); !added) {
                    return added;
                }
                apex = right;
                apex_index = right_index;
                left = apex;
                left_index = apex_index;
                i = apex_index;
                continue;
            }
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

FixedVec2 follow_path(Span<const FixedVec2> path, FixedVec2 position, Fixed speed, Fixed arrival,
                      Fixed tick_rate, u32& cursor) noexcept {
    const WideFixed arrival_squared = WideFixed::product(arrival, arrival);
    while (cursor < path.size() &&
           detmath::distance_squared(position, path[cursor]) <= arrival_squared) {
        ++cursor;
    }
    if (cursor >= path.size()) {
        return FixedVec2::zero();
    }
    return detmath::clamp_length((path[cursor] - position) * tick_rate, speed);
}

}  // namespace cy::movement
