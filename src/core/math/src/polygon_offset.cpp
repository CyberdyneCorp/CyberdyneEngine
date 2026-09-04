// Polygon offsetting. Task 3.1.5. See include/cy/core/math/geometry.h.
//
// Move every edge `distance` metres along its outward normal and rejoin the corners. Each corner is
// decided independently and locally, which is what makes the routine linear and predictable, and
// also what makes its one limitation unavoidable: a local rule cannot see that two corners have
// swept past each other.
//
// THE THREE CASES AT A CORNER, and why they are three rather than one:
//
//   * The offset edges still meet at a sensible point — extend both to their intersection. This is
//     a *miter*, and it is the answer at every corner of a gently-turning polygon.
//   * They meet at a point absurdly far away. At a sharp corner the miter length grows as
//     1/sin(θ/2) and goes to infinity as the corner closes, so a 1-degree spike offset by a
//     centimetre grows a 1-metre needle. `miter_limit` is the ratio at which that is cut off and a
//     *bevel* — a straight segment across the corner — is emitted instead. This is the same
//     quantity, with the same name and the same customary default of 4, that SVG, Cairo and every
//     stroking API use, deliberately: a reader who knows one knows this.
//   * The corner turns the other way. At a reflex corner the two offset edges cross rather than
//     diverge, their intersection is close by whatever the angle, and there is no arc to
//     approximate. `miter_limit` and `JoinStyle` do not apply, and applying them anyway would cut a
//     notch into the polygon.
//
// Which of the first two applies depends on the *sign* of the offset as well as the corner: shrink
// a polygon and its convex corners become the ones that close up. The single test
// `turn * distance > 0` is what captures that, and it is why the sign is not normalised away.
//
// WHAT THIS DOES NOT DO is remove self-intersections; the header says so and it is worth repeating
// here, next to the reason. Shrinking a polygon past the radius of its narrowest neck makes the two
// sides of the neck cross. Every vertex this routine returns is still individually correct — each
// is the right point for its own corner — and the ring they form has a figure-eight in it. Removing
// that is a boolean union of the offset segments' swept regions, an algorithm an order of magnitude
// larger than this file, and the shape of the answer would be a *set* of contours rather than one.
// A caller that must know can compare the sign of `polygon_signed_area2` before and after.

#include <cy/core/math/geometry.h>

#include <cmath>

namespace cy::geom {
namespace {

/// Below this, an edge has no direction and the corner rule has nothing to work with.
constexpr f32 kDegenerateEdge = 1e-12f;

/// Below this — as a sine, so as a fraction of a right angle — two consecutive edges are parallel
/// and the corner is not a corner.
constexpr f32 kStraightCorner = 1e-7f;

/// The outward normal of the edge running along `direction`, for a **counter-clockwise** polygon.
///
/// The rotation is by −90°, not +90°, and that is the whole of the winding requirement in the
/// interface: for a counter-clockwise ring the interior is to the left of every edge, so the
/// outward side is to the right. Reversing the input's winding without reversing this would offset
/// every polygon inward while reporting that it grew.
[[nodiscard]] Vec2 outward_normal(Vec2 direction) noexcept {
    return Vec2{direction.y, -direction.x};
}

/// Where two offset edges meet, given a point and a direction on each. The caller has already
/// established that they are not parallel.
[[nodiscard]] Vec2 intersect_lines(Vec2 a, Vec2 a_direction, Vec2 b, Vec2 b_direction,
                                   f32 turn) noexcept {
    const f32 t = cross(b - a, b_direction) / turn;
    return a + a_direction * t;
}

/// Emit into the caller's buffer, refusing to run past its end. One place, so that no corner case
/// below has to remember the check.
struct Sink {
    Vec2* out = nullptr;
    usize capacity = 0;
    usize written = 0;

    [[nodiscard]] bool push(Vec2 v) noexcept {
        if (written >= capacity) {
            return false;
        }
        out[written++] = v;
        return true;
    }
};

/// Everything the caller can get wrong before a single corner is looked at. Separate from the loop
/// so that the loop reads as the algorithm and this reads as the contract.
[[nodiscard]] Expected<void, Error> check_offset_arguments(const Vec2* vertices, usize count,
                                                           f32 miter_limit,
                                                           const Vec2* out_vertices,
                                                           usize out_capacity) noexcept {
    if (vertices == nullptr || out_vertices == nullptr) {
        return cy::fail(ErrorCode::InvalidArgument, "offset_polygon(): a null array");
    }
    if (count < 3) {
        return cy::fail(ErrorCode::InvalidArgument,
                        "offset_polygon(): a polygon needs at least three vertices");
    }
    if (out_capacity < 2 * count) {
        return cy::fail(ErrorCode::BufferTooSmall,
                        "offset_polygon(): out_vertices needs room for 2 * count — every corner "
                        "may bevel");
    }
    if (miter_limit < 1.0f) {
        return cy::fail(ErrorCode::InvalidArgument,
                        "offset_polygon(): a miter limit below 1 would bevel a straight edge");
    }
    if (polygon_signed_area2(vertices, count) <= 0.0f) {
        return cy::fail(ErrorCode::InvalidArgument,
                        "offset_polygon(): the polygon must wind counter-clockwise, so that a "
                        "positive distance means outward — reverse the vertex order");
    }
    return {};
}

/// The join at one corner, appended to `sink`. This is the whole of the algorithm; everything
/// around it is bookkeeping. The three cases are the ones the header comment sets out — a miter, a
/// bevel where the miter would run away, and a reflex corner where neither choice applies.
[[nodiscard]] Expected<void, Error> join_corner(Vec2 previous, Vec2 current, Vec2 next,
                                                f32 distance, JoinStyle join, f32 miter_limit,
                                                Sink& sink) noexcept {
    const Vec2 incoming = current - previous;
    const Vec2 outgoing = next - current;
    const f32 incoming_length = length(incoming);
    const f32 outgoing_length = length(outgoing);
    if (incoming_length < kDegenerateEdge || outgoing_length < kDegenerateEdge) {
        return cy::fail(ErrorCode::InvalidArgument,
                        "offset_polygon(): two consecutive vertices coincide, so an edge has no "
                        "direction to offset along — weld the polygon first");
    }

    const Vec2 incoming_direction = incoming / incoming_length;
    const Vec2 outgoing_direction = outgoing / outgoing_length;
    // The two candidate corner points: the end of the offset incoming edge and the start of the
    // offset outgoing one. At a straight corner they are the same point.
    const Vec2 arriving = current + outward_normal(incoming_direction) * distance;
    const Vec2 leaving = current + outward_normal(outgoing_direction) * distance;
    const f32 turn = cross(incoming_direction, outgoing_direction);

    bool fitted = false;
    if (std::fabs(turn) < kStraightCorner) {
        // Parallel. Either the edges continue straight — one point, and the two candidates coincide
        // — or the polygon doubles back on itself, where the offset edges are antiparallel, there
        // is no intersection at all, and the segment between the two candidates is the only thing
        // that can be drawn.
        const bool doubles_back = dot(incoming_direction, outgoing_direction) < 0.0f;
        fitted = sink.push(arriving) && (!doubles_back || sink.push(leaving));
    } else {
        const Vec2 miter =
            intersect_lines(arriving, incoming_direction, leaving, outgoing_direction, turn);
        // The corner opens outward exactly when the turn and the offset agree in sign; only then is
        // there an arc to cut, and only then do the join style and the limit have anything to say.
        const bool opens_outward = turn * distance > 0.0f;
        const f32 ratio = length(miter - current) / std::fabs(distance);
        const bool bevelled = opens_outward && (join == JoinStyle::Bevel || ratio > miter_limit);
        fitted = bevelled ? (sink.push(arriving) && sink.push(leaving)) : sink.push(miter);
    }
    if (!fitted) {
        return cy::fail(ErrorCode::BufferTooSmall, "offset_polygon(): out_vertices is too small");
    }
    return {};
}

}  // namespace

Expected<usize, Error> offset_polygon(const Vec2* vertices, usize count, f32 distance,
                                      JoinStyle join, f32 miter_limit, Vec2* out_vertices,
                                      usize out_capacity) noexcept {
    if (const Expected<void, Error> checked =
            check_offset_arguments(vertices, count, miter_limit, out_vertices, out_capacity);
        !checked) {
        return cy::make_unexpected(checked.error());
    }

    if (distance == 0.0f) {
        // Not a special case for speed but for meaning: with no offset there is no corner to
        // rejoin, every miter ratio is 0/0, and the answer is the input.
        for (usize i = 0; i < count; ++i) {
            out_vertices[i] = vertices[i];
        }
        return count;
    }

    Sink sink{out_vertices, out_capacity, 0};
    for (usize i = 0; i < count; ++i) {
        const Expected<void, Error> joined =
            join_corner(vertices[(i + count - 1) % count], vertices[i], vertices[(i + 1) % count],
                        distance, join, miter_limit, sink);
        if (!joined) {
            return cy::make_unexpected(joined.error());
        }
    }
    return sink.written;
}

}  // namespace cy::geom
