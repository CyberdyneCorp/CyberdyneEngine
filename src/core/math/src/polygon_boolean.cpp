// Union, intersection and difference of two simple polygons. Task 3.1.5.
// See include/cy/core/math/geometry.h.
//
// THE ALGORITHM is Greiner–Hormann, in three phases:
//
//   1. Intersect every subject edge with every clip edge. Each crossing becomes a *pair* of
//      vertices — one threaded into the subject's vertex ring, one into the clip's — that know
//      about each other.
//   2. Label every crossing an entry or an exit: walking the subject ring, the crossings alternate
//      between entering the clip polygon and leaving it, and which one comes first is decided by
//      whether the subject's own first vertex is inside the clip. Same again from the clip's side.
//   3. Walk. From an unvisited crossing, follow one ring until the next crossing, hop to that
//      crossing's twin in the other ring, and carry on until you are back where you started. That
//      closes one contour; repeat until no unvisited crossing is left.
//
// The three boolean operations are the *same* walk with the entry labels seeded differently, which
// is the whole elegance of the method: union starts by treating the outside as the inside, and a
// difference does so for one polygon and not the other.
//
// WHY DEGENERACIES ARE REFUSED RATHER THAN HANDLED. Phase 2's alternation is only true when the two
// boundaries cross transversally. A vertex lying exactly on the other polygon's edge, two collinear
// overlapping edges, a shared vertex — each of these makes a boundary *touch* without crossing, the
// alternation loses its footing, and the walk emits a contour that is short one piece or that never
// closes. The published extensions that handle these cases (Foster–Overfelt and its relatives) are
// a substantial amount of case analysis and are not here.
//
// What *is* here is detection. Every touching configuration is recognised while the crossings are
// computed and reported as `ErrorCode::Unsupported` naming what was found. That is the honest
// answer, and it is far better than the alternative: a boolean that silently drops a contour gives
// a caller no way to know, and the damage shows up as a hole in a navigation mesh or a missing
// piece of a destructible wall, three subsystems away from the cause.
//
// A NOTE ON THE TOLERANCES. Both epsilons below are relative — one to the parameter along an edge,
// one to the two edges' lengths — because an absolute one would mean something different for a
// 1-metre polygon and a 1-kilometre one, and this routine is used for both. They are deliberately
// tight: their job is to catch the exactly-degenerate case and the case so close to it that the
// answer would be arbitrary, not to snap near-misses together. Snapping is a caller's decision,
// made with `weld_vertices` and the caller's own idea of what "the same point" means.

#include <cy/core/math/geometry.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace cy::geom {
namespace {

/// A point in the precision the crossing computation runs in. The interface is `f32` on both sides;
/// only the arithmetic between them is widened, for the same reason as in delaunay.cpp — the
/// crossing parameter is a ratio of differences of products, and it decides which side of a
/// boundary a whole contour ends up on.
struct Vec2d {
    f64 x = 0.0;
    f64 y = 0.0;
};

[[nodiscard]] Vec2d to_double(Vec2 v) noexcept {
    return Vec2d{static_cast<f64>(v.x), static_cast<f64>(v.y)};
}

[[nodiscard]] Vec2 to_float(Vec2d v) noexcept {
    return Vec2{static_cast<f32>(v.x), static_cast<f32>(v.y)};
}

[[nodiscard]] Vec2d sub2(Vec2d a, Vec2d b) noexcept {
    return Vec2d{a.x - b.x, a.y - b.y};
}

[[nodiscard]] f64 cross2(Vec2d a, Vec2d b) noexcept {
    return (a.x * b.y) - (a.y * b.x);
}

[[nodiscard]] f64 dot2(Vec2d a, Vec2d b) noexcept {
    return (a.x * b.x) + (a.y * b.y);
}

/// How close to an edge's end a crossing may fall before it counts as *at* the end, as a fraction
/// of the edge. A crossing at a vertex is the degeneracy this whole file refuses.
constexpr f64 kParameterEpsilon = 1e-7;

/// How near-parallel two edges may be before the crossing parameter stops meaning anything,
/// relative to the product of their lengths.
constexpr f64 kParallelEpsilon = 1e-12;

/// A vertex of one of the two rings. Rings are circular doubly-linked lists held in one flat vector
/// and addressed by index, so that inserting a crossing is two pointer writes and no allocation
/// beyond the vector's own growth, and so that a reallocation cannot invalidate anything.
struct Node {
    Vec2d p{};
    u32 next = 0;
    u32 prev = 0;
    /// The twin of this crossing in the other ring, or `kNoNeighbour` for an original vertex.
    u32 neighbour = 0;
    bool intersection = false;
    /// Whether walking forward through this crossing enters the other polygon. Meaningless on an
    /// original vertex.
    bool entry = false;
    bool visited = false;
};

inline constexpr u32 kNoNeighbour = 0xFFFFFFFFu;

/// One crossing of a subject edge with a clip edge, before it is threaded into the rings.
struct Crossing {
    Vec2d p{};
    u32 subject_edge = 0;
    u32 clip_edge = 0;
    /// Position along each edge, in (0, 1). The rings are ordered by these.
    f64 alpha_subject = 0.0;
    f64 alpha_clip = 0.0;
    u32 subject_node = 0;
    u32 clip_node = 0;
};

/// Even-odd containment, in the predicate precision, over a polygon given as `Vec2d`.
///
/// `point_in_polygon` in geometry.cpp answers the same question for `Vec2`; this is not a
/// duplicate of it so much as the same test at the precision the rest of this file works in.
/// Crucially it is only ever asked about a vertex that is *not* on the other boundary — the
/// degeneracy check guarantees that — so the boundary tie it would otherwise have to resolve
/// cannot arise here.
[[nodiscard]] bool contains_point(const std::vector<Vec2d>& polygon, Vec2d point) noexcept {
    bool inside = false;
    const usize count = polygon.size();
    for (usize i = 0, j = count - 1; i < count; j = i++) {
        const Vec2d a = polygon[i];
        const Vec2d b = polygon[j];
        if ((a.y > point.y) != (b.y > point.y)) {
            const f64 crossing_x = a.x + ((point.y - a.y) / (b.y - a.y) * (b.x - a.x));
            if (point.x < crossing_x) {
                inside = !inside;
            }
        }
    }
    return inside;
}

/// What one pair of edges did to each other.
enum class EdgePairResult : u32 {
    /// They do not meet, or meet only outside both segments.
    Apart,
    /// They cross cleanly at one interior point of each.
    Crosses,
    /// They touch at a vertex, overlap along a line, or cross so close to a vertex that which side
    /// it falls on is arbitrary. This is what the routine refuses; see the header comment.
    Degenerate,
};

/// Whether two collinear, parallel segments share more than nothing.
[[nodiscard]] bool collinear_overlap(Vec2d a0, Vec2d a1, Vec2d b0, Vec2d b1) noexcept {
    const Vec2d direction = sub2(a1, a0);
    const f64 length_squared = dot2(direction, direction);
    if (length_squared <= 0.0) {
        return false;
    }
    const f64 t0 = dot2(sub2(b0, a0), direction) / length_squared;
    const f64 t1 = dot2(sub2(b1, a0), direction) / length_squared;
    const f64 low = std::fmin(t0, t1);
    const f64 high = std::fmax(t0, t1);
    return high > kParameterEpsilon && low < 1.0 - kParameterEpsilon;
}

/// The crossing of two segments, if they have one this algorithm can use.
[[nodiscard]] EdgePairResult intersect_edges(Vec2d s0, Vec2d s1, Vec2d c0, Vec2d c1,
                                             f64& alpha_subject, f64& alpha_clip) noexcept {
    const Vec2d subject_direction = sub2(s1, s0);
    const Vec2d clip_direction = sub2(c1, c0);
    const f64 denominator = cross2(subject_direction, clip_direction);

    // Near-parallel is decided against the product of the lengths, so the test means "the angle
    // between them is tiny" rather than "these are short edges".
    const f64 scale = std::sqrt(dot2(subject_direction, subject_direction)) *
                      std::sqrt(dot2(clip_direction, clip_direction));
    if (std::fabs(denominator) <= kParallelEpsilon * scale) {
        // Parallel. Collinear and overlapping is a degeneracy; anything else simply misses.
        const f64 offset = cross2(sub2(c0, s0), subject_direction);
        const bool collinear = std::fabs(offset) <= kParallelEpsilon * scale;
        return collinear && collinear_overlap(s0, s1, c0, c1) ? EdgePairResult::Degenerate
                                                              : EdgePairResult::Apart;
    }

    const Vec2d to_clip = sub2(c0, s0);
    alpha_subject = cross2(to_clip, clip_direction) / denominator;
    alpha_clip = cross2(to_clip, subject_direction) / denominator;

    const bool subject_outside =
        alpha_subject < -kParameterEpsilon || alpha_subject > 1.0 + kParameterEpsilon;
    const bool clip_outside =
        alpha_clip < -kParameterEpsilon || alpha_clip > 1.0 + kParameterEpsilon;
    if (subject_outside || clip_outside) {
        return EdgePairResult::Apart;
    }

    const bool subject_at_end =
        alpha_subject <= kParameterEpsilon || alpha_subject >= 1.0 - kParameterEpsilon;
    const bool clip_at_end =
        alpha_clip <= kParameterEpsilon || alpha_clip >= 1.0 - kParameterEpsilon;
    if (subject_at_end || clip_at_end) {
        return EdgePairResult::Degenerate;
    }
    return EdgePairResult::Crosses;
}

/// Every clean crossing of the two boundaries, or the degeneracy that stopped the search.
[[nodiscard]] Expected<std::vector<Crossing>, Error> find_crossings(
    const std::vector<Vec2d>& subject, const std::vector<Vec2d>& clip) {
    std::vector<Crossing> crossings;
    const usize subject_count = subject.size();
    const usize clip_count = clip.size();

    for (usize i = 0; i < subject_count; ++i) {
        const Vec2d s0 = subject[i];
        const Vec2d s1 = subject[(i + 1) % subject_count];
        for (usize j = 0; j < clip_count; ++j) {
            const Vec2d c0 = clip[j];
            const Vec2d c1 = clip[(j + 1) % clip_count];

            f64 alpha_subject = 0.0;
            f64 alpha_clip = 0.0;
            switch (intersect_edges(s0, s1, c0, c1, alpha_subject, alpha_clip)) {
                case EdgePairResult::Apart:
                    continue;
                case EdgePairResult::Degenerate:
                    return cy::fail(
                        ErrorCode::Unsupported,
                        "polygon_boolean(): the boundaries touch without crossing — a shared "
                        "vertex, a vertex on an edge, or two collinear overlapping edges. "
                        "Greiner-Hormann is undefined there; perturb or weld the input");
                case EdgePairResult::Crosses:
                    break;
            }

            Crossing crossing;
            crossing.p = Vec2d{s0.x + (alpha_subject * (s1.x - s0.x)),
                               s0.y + (alpha_subject * (s1.y - s0.y))};
            crossing.subject_edge = static_cast<u32>(i);
            crossing.clip_edge = static_cast<u32>(j);
            crossing.alpha_subject = alpha_subject;
            crossing.alpha_clip = alpha_clip;
            crossings.push_back(crossing);
        }
    }
    return crossings;
}

/// The two rings, laid out back to back: the subject occupies `[0, subject_count)` and the clip
/// `[subject_count, subject_count + clip_count)`, with crossings appended after both.
[[nodiscard]] std::vector<Node> build_rings(const std::vector<Vec2d>& subject,
                                            const std::vector<Vec2d>& clip) {
    std::vector<Node> nodes;
    nodes.reserve(subject.size() + clip.size());

    const u32 subject_count = static_cast<u32>(subject.size());
    const u32 clip_count = static_cast<u32>(clip.size());
    for (u32 i = 0; i < subject_count; ++i) {
        Node node;
        node.p = subject[i];
        node.next = (i + 1) % subject_count;
        node.prev = (i + subject_count - 1) % subject_count;
        node.neighbour = kNoNeighbour;
        nodes.push_back(node);
    }
    for (u32 i = 0; i < clip_count; ++i) {
        Node node;
        node.p = clip[i];
        node.next = subject_count + ((i + 1) % clip_count);
        node.prev = subject_count + ((i + clip_count - 1) % clip_count);
        node.neighbour = kNoNeighbour;
        nodes.push_back(node);
    }
    return nodes;
}

/// Splice one new crossing node into a ring immediately after `after`.
[[nodiscard]] u32 splice_after(std::vector<Node>& nodes, u32 after, Vec2d p) {
    Node node;
    node.p = p;
    node.intersection = true;
    node.neighbour = kNoNeighbour;
    node.prev = after;
    node.next = nodes[after].next;

    const u32 index = static_cast<u32>(nodes.size());
    nodes.push_back(node);
    nodes[nodes[index].next].prev = index;
    nodes[after].next = index;
    return index;
}

/// Thread every crossing into both rings, in order along each edge, and link each pair of twins.
void thread_crossings(std::vector<Node>& nodes, std::vector<Crossing>& crossings,
                      u32 subject_count) {
    std::vector<u32> order(crossings.size());
    for (u32 i = 0; i < static_cast<u32>(crossings.size()); ++i) {
        order[i] = i;
    }

    // Along the subject: edge by edge, and within an edge by position along it. Inserting out of
    // order would put the crossings in the ring in the wrong sequence, and the entry/exit
    // alternation of phase 2 reads that sequence as the truth.
    std::ranges::sort(order, [&crossings](u32 lhs, u32 rhs) noexcept {
        const Crossing& a = crossings[lhs];
        const Crossing& b = crossings[rhs];
        return a.subject_edge != b.subject_edge ? a.subject_edge < b.subject_edge
                                                : a.alpha_subject < b.alpha_subject;
    });
    u32 previous_edge = 0xFFFFFFFFu;
    u32 cursor = 0;
    for (const u32 index : order) {
        Crossing& crossing = crossings[index];
        if (crossing.subject_edge != previous_edge) {
            previous_edge = crossing.subject_edge;
            cursor = crossing.subject_edge;
        }
        cursor = splice_after(nodes, cursor, crossing.p);
        crossing.subject_node = cursor;
    }

    std::ranges::sort(order, [&crossings](u32 lhs, u32 rhs) noexcept {
        const Crossing& a = crossings[lhs];
        const Crossing& b = crossings[rhs];
        return a.clip_edge != b.clip_edge ? a.clip_edge < b.clip_edge : a.alpha_clip < b.alpha_clip;
    });
    previous_edge = 0xFFFFFFFFu;
    for (const u32 index : order) {
        Crossing& crossing = crossings[index];
        if (crossing.clip_edge != previous_edge) {
            previous_edge = crossing.clip_edge;
            cursor = subject_count + crossing.clip_edge;
        }
        cursor = splice_after(nodes, cursor, crossing.p);
        crossing.clip_node = cursor;
    }
    for (const Crossing& crossing : crossings) {
        nodes[crossing.subject_node].neighbour = crossing.clip_node;
        nodes[crossing.clip_node].neighbour = crossing.subject_node;
    }
}

/// Phase 2. Walk each ring from its first original vertex and flip the flag at every crossing.
///
/// The seeds are the whole of the operation's identity: intersection keeps both polygons' insides,
/// union keeps both outsides, and a difference keeps the subject's outside and the clip's inside —
/// which is what makes the walk trace the clip boundary backwards and cut the bite out.
void label_entries(std::vector<Node>& nodes, const std::vector<Vec2d>& subject,
                   const std::vector<Vec2d>& clip, u32 subject_count, BooleanOp op) {
    bool subject_entry = op == BooleanOp::Intersection;
    bool clip_entry = op == BooleanOp::Intersection || op == BooleanOp::Difference;

    subject_entry = subject_entry != contains_point(clip, nodes[0].p);
    clip_entry = clip_entry != contains_point(subject, nodes[subject_count].p);

    u32 current = 0;
    do {
        if (nodes[current].intersection) {
            nodes[current].entry = subject_entry;
            subject_entry = !subject_entry;
        }
        current = nodes[current].next;
    } while (current != 0);

    current = subject_count;
    do {
        if (nodes[current].intersection) {
            nodes[current].entry = clip_entry;
            clip_entry = !clip_entry;
        }
        current = nodes[current].next;
    } while (current != subject_count);
}

/// Twice the signed area of a contour, for the winding fix-up below.
[[nodiscard]] f64 signed_area2(const std::vector<Vec2d>& contour) noexcept {
    f64 total = 0.0;
    const usize count = contour.size();
    for (usize i = 0, j = count - 1; i < count; j = i++) {
        total += cross2(contour[j], contour[i]);
    }
    return total;
}

/// Phase 3, for one contour. Returns false when the walk failed to close, which can only happen if
/// phase 1 or 2 left the rings inconsistent, and is reported rather than swallowed.
[[nodiscard]] bool trace_contour(std::vector<Node>& nodes, u32 start, std::vector<Vec2d>& contour) {
    const usize step_limit = (4 * nodes.size()) + 8;
    usize steps = 0;

    contour.clear();
    contour.push_back(nodes[start].p);

    u32 current = start;
    do {
        // Marking the twin as well as the node is what makes the walk terminate: arriving back at
        // the start from the *other* ring is the ordinary way a contour closes, and a walk that
        // only marked the node it stood on would go round for ever. Marking happens here, at the
        // node about to be walked away from, and NOT at the node just arrived at — marking the
        // arrival would mark its twin too, which is the node the very next line hops to, and the
        // walk would stop after one segment with a contour that is a fragment of the answer. That
        // bug produced a plausible-looking four-vertex polygon for the union of two squares, which
        // is exactly the kind of wrong answer this file's tests exist to catch.
        nodes[current].visited = true;
        if (nodes[current].neighbour != kNoNeighbour) {
            nodes[nodes[current].neighbour].visited = true;
        }

        const bool forward = nodes[current].entry;
        do {
            current = forward ? nodes[current].next : nodes[current].prev;
            contour.push_back(nodes[current].p);
            if (++steps > step_limit) {
                return false;
            }
        } while (!nodes[current].intersection);

        current = nodes[current].neighbour;
    } while (!nodes[current].visited && ++steps <= step_limit);

    if (steps > step_limit) {
        return false;
    }
    // The walk ends by arriving back at the start, whose point is already at the front.
    contour.pop_back();
    if (signed_area2(contour) < 0.0) {
        std::ranges::reverse(contour);
    }
    return true;
}

/// Where a completed contour is written, with the capacity checks in one place.
struct Sink {
    Vec2* vertices = nullptr;
    usize vertex_capacity = 0;
    u32* contour_sizes = nullptr;
    usize contour_capacity = 0;
    BooleanResult result{};

    [[nodiscard]] Expected<void, Error> add(const std::vector<Vec2d>& contour) {
        if (contour.size() < 3) {
            return {};  // A sliver with no area is not a contour anybody can use.
        }
        if (result.contour_count >= contour_capacity) {
            return cy::fail(ErrorCode::BufferTooSmall,
                            "polygon_boolean(): out_contour_sizes is too small");
        }
        if (result.vertex_count + contour.size() > vertex_capacity) {
            return cy::fail(ErrorCode::BufferTooSmall,
                            "polygon_boolean(): out_vertices is too small");
        }
        for (const Vec2d& p : contour) {
            vertices[result.vertex_count++] = to_float(p);
        }
        contour_sizes[result.contour_count++] = static_cast<u32>(contour.size());
        return {};
    }
};

/// The answer when the two boundaries never cross: one polygon is inside the other, or they are
/// disjoint. Phase 3 has nothing to walk from in that case, and each operation's answer is one of
/// the inputs, both of them, or nothing.
[[nodiscard]] Expected<void, Error> resolve_without_crossings(BooleanOp op,
                                                              const std::vector<Vec2d>& subject,
                                                              const std::vector<Vec2d>& clip,
                                                              Sink& sink) {
    const bool subject_inside_clip = contains_point(clip, subject[0]);
    const bool clip_inside_subject = contains_point(subject, clip[0]);

    switch (op) {
        case BooleanOp::Union:
            if (subject_inside_clip) {
                return sink.add(clip);
            }
            if (clip_inside_subject) {
                return sink.add(subject);
            }
            if (const Expected<void, Error> added = sink.add(subject); !added) {
                return added;
            }
            return sink.add(clip);

        case BooleanOp::Intersection:
            if (subject_inside_clip) {
                return sink.add(subject);
            }
            if (clip_inside_subject) {
                return sink.add(clip);
            }
            return {};  // Disjoint: an empty intersection, and not an error.

        case BooleanOp::Difference:
            if (subject_inside_clip) {
                return {};  // Entirely removed.
            }
            if (clip_inside_subject) {
                return cy::fail(ErrorCode::Unsupported,
                                "polygon_boolean(): the difference is an annulus — the clip lies "
                                "strictly inside the subject, and this interface cannot say that "
                                "one contour is a hole in another");
            }
            return sink.add(subject);
    }
    return cy::fail(ErrorCode::InvalidArgument, "polygon_boolean(): unknown operation");
}

/// A polygon copied into the predicate precision, with the winding it was given. Winding does not
/// matter to the algorithm — the entry labels are derived from containment, not from orientation —
/// so the input is not normalised and the output's winding is fixed up per contour instead.
[[nodiscard]] std::vector<Vec2d> to_double_polygon(const Vec2* vertices, usize count) {
    std::vector<Vec2d> polygon;
    polygon.reserve(count);
    for (usize i = 0; i < count; ++i) {
        polygon.push_back(to_double(vertices[i]));
    }
    return polygon;
}

}  // namespace

Expected<BooleanResult, Error> polygon_boolean(BooleanOp op, const Vec2* subject,
                                               usize subject_count, const Vec2* clip,
                                               usize clip_count, Vec2* out_vertices,
                                               usize out_vertex_capacity, u32* out_contour_sizes,
                                               usize out_contour_capacity) noexcept {
    if (subject == nullptr || clip == nullptr || out_vertices == nullptr ||
        out_contour_sizes == nullptr) {
        return cy::fail(ErrorCode::InvalidArgument, "polygon_boolean(): a null array");
    }
    if (subject_count < 3 || clip_count < 3) {
        return cy::fail(ErrorCode::InvalidArgument,
                        "polygon_boolean(): both polygons need at least three vertices");
    }

    const std::vector<Vec2d> subject_polygon = to_double_polygon(subject, subject_count);
    const std::vector<Vec2d> clip_polygon = to_double_polygon(clip, clip_count);

    Sink sink;
    sink.vertices = out_vertices;
    sink.vertex_capacity = out_vertex_capacity;
    sink.contour_sizes = out_contour_sizes;
    sink.contour_capacity = out_contour_capacity;

    Expected<std::vector<Crossing>, Error> found = find_crossings(subject_polygon, clip_polygon);
    if (!found) {
        return cy::make_unexpected(found.error());
    }
    std::vector<Crossing> crossings = std::move(found.value());

    if (crossings.empty()) {
        const Expected<void, Error> resolved =
            resolve_without_crossings(op, subject_polygon, clip_polygon, sink);
        if (!resolved) {
            return cy::make_unexpected(resolved.error());
        }
        return sink.result;
    }
    // Two simple boundaries cross an even number of times. An odd count means a crossing was
    // counted at a touch the degeneracy check should have caught, and continuing would produce a
    // walk that cannot close.
    if (crossings.size() % 2 != 0) {
        return cy::fail(ErrorCode::Unsupported,
                        "polygon_boolean(): an odd number of crossings — the boundaries touch "
                        "somewhere this routine could not classify");
    }

    std::vector<Node> nodes = build_rings(subject_polygon, clip_polygon);
    nodes.reserve(nodes.size() + (2 * crossings.size()));
    thread_crossings(nodes, crossings, static_cast<u32>(subject_count));
    label_entries(nodes, subject_polygon, clip_polygon, static_cast<u32>(subject_count), op);

    std::vector<Vec2d> contour;
    for (u32 i = 0; i < static_cast<u32>(nodes.size()); ++i) {
        if (!nodes[i].intersection || nodes[i].visited) {
            continue;
        }
        if (!trace_contour(nodes, i, contour)) {
            return cy::fail(ErrorCode::Internal,
                            "polygon_boolean(): a contour walk did not close — the crossing rings "
                            "are inconsistent");
        }
        if (const Expected<void, Error> added = sink.add(contour); !added) {
            return cy::make_unexpected(added.error());
        }
    }
    return sink.result;
}

}  // namespace cy::geom
