// Geometry utilities. Task 3.1.5. See include/cy/core/math/geometry.h.

#include <cy/core/math/geometry.h>

#include <cy/core/base/assert.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace cy::geom {
namespace {

/// Below this, a ray is treated as parallel to a plane or a slab. Chosen against a direction that
/// is unit-length, which every routine here requires.
constexpr f32 kParallelEpsilon = 1e-8f;

/// The sign of the area of the triangle (a, b, c): positive when c is left of the line a→b.
[[nodiscard]] f32 orient2d(Vec2 a, Vec2 b, Vec2 c) noexcept {
    return cross(b - a, c - a);
}

/// "No vertex within tolerance", for the welder. Not a valid vertex index, and not zero: zero is
/// the first unique vertex every mesh has.
inline constexpr u32 kNoWeldMatch = 0xFFFFFFFFu;

}  // namespace

// --- Ray casts
// ------------------------------------------------------------------------------------

bool ray_aabb(const Ray& ray, const Aabb& box, f32 max_distance, f32& t_min, f32& t_max) noexcept {
    f32 enter = 0.0f;
    f32 exit = max_distance;
    for (usize axis = 0; axis < 3; ++axis) {
        const f32 direction = ray.direction[axis];
        const f32 origin = ray.origin[axis];
        if (std::fabs(direction) < kParallelEpsilon) {
            // Parallel to this slab: the ray either lies within it for its whole length or misses
            // the box entirely, and the other two axes decide nothing.
            if (origin < box.min[axis] || origin > box.max[axis]) {
                return false;
            }
            continue;
        }
        const f32 inv = 1.0f / direction;
        f32 near_t = (box.min[axis] - origin) * inv;
        f32 far_t = (box.max[axis] - origin) * inv;
        if (near_t > far_t) {
            std::swap(near_t, far_t);
        }
        enter = math::max(enter, near_t);
        exit = math::min(exit, far_t);
        if (enter > exit) {
            return false;
        }
    }
    t_min = enter;
    t_max = exit;
    return true;
}

bool ray_sphere(const Ray& ray, const Sphere& sphere, f32 max_distance, f32& t) noexcept {
    const Vec3 to_center = ray.origin - sphere.center;
    const f32 projection = dot(to_center, ray.direction);
    const f32 outside = dot(to_center, to_center) - sphere.radius * sphere.radius;
    // Origin outside and pointing away: no root can be non-negative, and this rejects most misses
    // before the discriminant is computed.
    if (outside > 0.0f && projection > 0.0f) {
        return false;
    }
    const f32 discriminant = projection * projection - outside;
    if (discriminant < 0.0f) {
        return false;
    }
    const f32 root = std::sqrt(discriminant);
    f32 hit = -projection - root;
    if (hit < 0.0f) {
        // The near root is behind the origin, so the origin is inside: take the exit point.
        hit = -projection + root;
    }
    if (hit < 0.0f || hit > max_distance) {
        return false;
    }
    t = hit;
    return true;
}

bool ray_plane(const Ray& ray, const Plane& plane, f32 max_distance, f32& t) noexcept {
    const f32 denominator = dot(plane.normal, ray.direction);
    if (std::fabs(denominator) < kParallelEpsilon) {
        return false;
    }
    const f32 hit = -plane.signed_distance(ray.origin) / denominator;
    if (hit < 0.0f || hit > max_distance) {
        return false;
    }
    t = hit;
    return true;
}

bool ray_triangle(const Ray& ray, Vec3 v0, Vec3 v1, Vec3 v2, f32 max_distance, bool cull_back_faces,
                  TriangleHit& hit) noexcept {
    // Möller–Trumbore. The barycentric coordinates fall out of the same determinants the
    // intersection test needs, which is why this is the formulation everyone uses for picking.
    const Vec3 edge1 = v1 - v0;
    const Vec3 edge2 = v2 - v0;
    const Vec3 pvec = cross(ray.direction, edge2);
    const f32 determinant = dot(edge1, pvec);

    const bool back_face = determinant < 0.0f;
    if (cull_back_faces && back_face) {
        return false;
    }
    if (std::fabs(determinant) < kParallelEpsilon) {
        return false;
    }

    const f32 inv_determinant = 1.0f / determinant;
    const Vec3 tvec = ray.origin - v0;
    const f32 u = dot(tvec, pvec) * inv_determinant;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const Vec3 qvec = cross(tvec, edge1);
    const f32 v = dot(ray.direction, qvec) * inv_determinant;
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    const f32 t = dot(edge2, qvec) * inv_determinant;
    if (t < 0.0f || t > max_distance) {
        return false;
    }
    hit = TriangleHit{t, u, v, back_face};
    return true;
}

// --- Closest points
// ---------------------------------------------------------------------------------

Vec3 closest_point_on_segment(Vec3 p, Vec3 start, Vec3 end) noexcept {
    const Vec3 direction = end - start;
    const f32 length_sq = dot(direction, direction);
    if (length_sq < math::kSmallLength) {
        return start;
    }
    const f32 t = math::saturate(dot(p - start, direction) / length_sq);
    return start + direction * t;
}

void closest_points_segment_segment(Vec3 a_start, Vec3 a_end, Vec3 b_start, Vec3 b_end, f32& s,
                                    f32& t, Vec3& point_a, Vec3& point_b) noexcept {
    // Ericson, Real-Time Collision Detection, §5.1.9. The structure is: solve the unconstrained
    // problem, then clamp each parameter to its segment and re-solve the other, which is what makes
    // the parallel and degenerate cases fall out instead of needing branches of their own.
    const Vec3 d1 = a_end - a_start;
    const Vec3 d2 = b_end - b_start;
    const Vec3 r = a_start - b_start;
    const f32 a = dot(d1, d1);
    const f32 e = dot(d2, d2);
    const f32 f = dot(d2, r);

    if (a < math::kSmallLength && e < math::kSmallLength) {
        s = 0.0f;
        t = 0.0f;
        point_a = a_start;
        point_b = b_start;
        return;
    }
    if (a < math::kSmallLength) {
        s = 0.0f;
        t = math::saturate(f / e);
    } else {
        const f32 c = dot(d1, r);
        if (e < math::kSmallLength) {
            t = 0.0f;
            s = math::saturate(-c / a);
        } else {
            const f32 b = dot(d1, d2);
            const f32 denominator = a * e - b * b;
            // Zero denominator means the segments are parallel: any s does, so pick the start.
            s = denominator != 0.0f ? math::saturate((b * f - c * e) / denominator) : 0.0f;
            t = (b * s + f) / e;
            // Clamping t may invalidate s, so recompute it against the clamped t.
            if (t < 0.0f) {
                t = 0.0f;
                s = math::saturate(-c / a);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = math::saturate((b - c) / a);
            }
        }
    }
    point_a = a_start + d1 * s;
    point_b = b_start + d2 * t;
}

Vec3 closest_point_on_triangle(Vec3 p, Vec3 v0, Vec3 v1, Vec3 v2) noexcept {
    // Ericson, §5.1.5: the seven Voronoi regions of a triangle, tested in an order that lets each
    // test reuse the previous one's dot products.
    const Vec3 ab = v1 - v0;
    const Vec3 ac = v2 - v0;
    const Vec3 ap = p - v0;
    const f32 d1 = dot(ab, ap);
    const f32 d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        return v0;
    }

    const Vec3 bp = p - v1;
    const f32 d3 = dot(ab, bp);
    const f32 d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        return v1;
    }

    const f32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        return v0 + ab * (d1 / (d1 - d3));
    }

    const Vec3 cp = p - v2;
    const f32 d5 = dot(ab, cp);
    const f32 d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        return v2;
    }

    const f32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        return v0 + ac * (d2 / (d2 - d6));
    }

    const f32 va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        return v1 + (v2 - v1) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }

    const f32 denominator = 1.0f / (va + vb + vc);
    return v0 + ab * (vb * denominator) + ac * (vc * denominator);
}

// --- Polygons
// ------------------------------------------------------------------------------------------

bool point_in_polygon(Vec2 point, const Vec2* vertices, usize count) noexcept {
    if (vertices == nullptr || count < 3) {
        return false;
    }
    // Crossing number: count the edges a ray from `point` in +X crosses. The `>` / `<=` asymmetry
    // on the y comparison is what makes a vertex exactly at the ray's height count once rather than
    // twice or not at all.
    bool inside = false;
    for (usize i = 0, j = count - 1; i < count; j = i++) {
        const Vec2 a = vertices[i];
        const Vec2 b = vertices[j];
        if ((a.y > point.y) != (b.y > point.y)) {
            const f32 crossing_x = a.x + (point.y - a.y) / (b.y - a.y) * (b.x - a.x);
            if (point.x < crossing_x) {
                inside = !inside;
            }
        }
    }
    return inside;
}

f32 polygon_signed_area2(const Vec2* vertices, usize count) noexcept {
    if (vertices == nullptr || count < 3) {
        return 0.0f;
    }
    f32 total = 0.0f;
    for (usize i = 0, j = count - 1; i < count; j = i++) {
        total += cross(vertices[j], vertices[i]);
    }
    return total;
}

Expected<usize, Error> convex_hull_2d(const Vec2* points, usize count, u32* out_indices,
                                      usize out_capacity, u32* scratch) noexcept {
    if (count == 0) {
        return usize{0};
    }
    if (points == nullptr || out_indices == nullptr || scratch == nullptr) {
        return cy::fail(ErrorCode::InvalidArgument, "convex_hull_2d(): a null array");
    }
    if (out_capacity < count + 1) {
        return cy::fail(ErrorCode::BufferTooSmall,
                        "convex_hull_2d(): out_indices needs room for count + 1");
    }
    if (count < 3) {
        for (usize i = 0; i < count; ++i) {
            out_indices[i] = static_cast<u32>(i);
        }
        return count;
    }

    for (usize i = 0; i < count; ++i) {
        scratch[i] = static_cast<u32>(i);
    }
    std::sort(scratch, scratch + count, [points](u32 a, u32 b) noexcept {
        const Vec2 pa = points[a];
        const Vec2 pb = points[b];
        return pa.x != pb.x ? pa.x < pb.x : pa.y < pb.y;
    });

    // Andrew's monotone chain: the lower hull left to right, then the upper hull right to left.
    // `<= 0` on the turn test drops collinear points, which is what makes the result minimal.
    usize k = 0;
    for (usize i = 0; i < count; ++i) {
        const u32 index = scratch[i];
        while (k >= 2 && orient2d(points[out_indices[k - 2]], points[out_indices[k - 1]],
                                  points[index]) <= 0.0f) {
            --k;
        }
        out_indices[k++] = index;
    }
    const usize lower_end = k + 1;
    for (usize i = count - 1; i-- > 0;) {
        const u32 index = scratch[i];
        while (k >= lower_end && orient2d(points[out_indices[k - 2]], points[out_indices[k - 1]],
                                          points[index]) <= 0.0f) {
            --k;
        }
        out_indices[k++] = index;
    }
    // The last entry is the starting point repeated, which closes the loop and is not part of the
    // vertex set.
    return k - 1;
}

Expected<usize, Error> triangulate_polygon(const Vec2* vertices, usize count, u32* out_indices,
                                           usize out_capacity) noexcept {
    if (vertices == nullptr || out_indices == nullptr) {
        return cy::fail(ErrorCode::InvalidArgument, "triangulate_polygon(): a null array");
    }
    if (count < 3) {
        return cy::fail(ErrorCode::InvalidArgument,
                        "triangulate_polygon(): a polygon needs at least three vertices");
    }
    const usize needed = 3 * (count - 2);
    if (out_capacity < needed) {
        return cy::fail(ErrorCode::BufferTooSmall,
                        "triangulate_polygon(): out_indices needs room for 3 * (count - 2)");
    }

    // Work in counter-clockwise order whatever the input winding, so the ear test has one sense.
    std::vector<u32> remaining;
    remaining.reserve(count);
    const bool counter_clockwise = polygon_signed_area2(vertices, count) > 0.0f;
    for (usize i = 0; i < count; ++i) {
        remaining.push_back(static_cast<u32>(counter_clockwise ? i : count - 1 - i));
    }

    usize written = 0;
    // Each successful clip removes one vertex, so `count` failures in a row means no ear exists and
    // the polygon is not simple. Counting failures rather than iterations is what turns an infinite
    // loop into a diagnosable error.
    usize failures = 0;
    usize at = 0;
    while (remaining.size() > 3) {
        if (failures > remaining.size()) {
            return cy::fail(
                ErrorCode::InvalidArgument,
                "triangulate_polygon(): no ear found — the polygon is self-intersecting "
                "or degenerate");
        }
        const usize size = remaining.size();
        const u32 prev = remaining[(at + size - 1) % size];
        const u32 current = remaining[at % size];
        const u32 next = remaining[(at + 1) % size];

        const Vec2 a = vertices[prev];
        const Vec2 b = vertices[current];
        const Vec2 c = vertices[next];

        bool is_ear = orient2d(a, b, c) > 0.0f;
        if (is_ear) {
            // An ear must also be empty: no other vertex of the polygon inside the candidate
            // triangle. Without this test a polygon with a notch is triangulated across the notch.
            for (const u32 other : remaining) {
                if (other == prev || other == current || other == next) {
                    continue;
                }
                const Vec2 p = vertices[other];
                if (orient2d(a, b, p) >= 0.0f && orient2d(b, c, p) >= 0.0f &&
                    orient2d(c, a, p) >= 0.0f) {
                    is_ear = false;
                    break;
                }
            }
        }

        if (!is_ear) {
            ++failures;
            at = (at + 1) % size;
            continue;
        }

        out_indices[written++] = prev;
        out_indices[written++] = current;
        out_indices[written++] = next;
        remaining.erase(remaining.begin() + static_cast<isize>(at % size));
        failures = 0;
        if (at >= remaining.size()) {
            at = 0;
        }
    }

    out_indices[written++] = remaining[0];
    out_indices[written++] = remaining[1];
    out_indices[written++] = remaining[2];
    return written;
}

Expected<usize, Error> clip_polygon_by_plane(const Vec3* vertices, usize count, const Plane& plane,
                                             Vec3* out_vertices, usize out_capacity) noexcept {
    if (count == 0) {
        return usize{0};
    }
    if (vertices == nullptr || out_vertices == nullptr) {
        return cy::fail(ErrorCode::InvalidArgument, "clip_polygon_by_plane(): a null array");
    }
    if (out_capacity < count + 1) {
        return cy::fail(ErrorCode::BufferTooSmall,
                        "clip_polygon_by_plane(): out_vertices needs room for count + 1");
    }

    usize written = 0;
    Vec3 previous = vertices[count - 1];
    f32 previous_distance = plane.signed_distance(previous);
    for (usize i = 0; i < count; ++i) {
        const Vec3 current = vertices[i];
        const f32 current_distance = plane.signed_distance(current);
        // The edge crosses the plane when the two distances differ in sign: emit the crossing point
        // before the vertex, so the output stays in order.
        if ((previous_distance < 0.0f) != (current_distance < 0.0f)) {
            const f32 t = previous_distance / (previous_distance - current_distance);
            out_vertices[written++] = previous + (current - previous) * t;
        }
        if (current_distance >= 0.0f) {
            out_vertices[written++] = current;
        }
        previous = current;
        previous_distance = current_distance;
    }
    return written;
}

namespace {

/// Copy a polygon straight through, for the cases with nothing to clip against.
[[nodiscard]] Expected<usize, Error> copy_polygon(const Vec3* vertices, usize count, Vec3* out,
                                                  usize capacity) noexcept {
    if (count > capacity) {
        return cy::fail(ErrorCode::BufferTooSmall, "clip_polygon_by_planes(): buffer too small");
    }
    for (usize i = 0; i < count; ++i) {
        out[i] = vertices[i];
    }
    return count;
}

}  // namespace

Expected<usize, Error> clip_polygon_by_planes(const Vec3* vertices, usize count,
                                              const Plane* planes, usize plane_count,
                                              Vec3* out_vertices, Vec3* scratch,
                                              usize buffer_capacity) noexcept {
    if (count == 0 || plane_count == 0) {
        return copy_polygon(vertices, count, out_vertices, buffer_capacity);
    }
    if (vertices == nullptr || planes == nullptr || out_vertices == nullptr || scratch == nullptr) {
        return cy::fail(ErrorCode::InvalidArgument, "clip_polygon_by_planes(): a null array");
    }
    if (buffer_capacity < count + plane_count) {
        return cy::fail(ErrorCode::BufferTooSmall,
                        "clip_polygon_by_planes(): each buffer needs room for count + plane_count");
    }

    // Ping-pong so that no plane's output aliases its own input. Each pass swaps the two buffers,
    // so after an even number of planes the result is back where it started — which is why the
    // initial assignment depends on the parity: it arranges for the last write to land in
    // `out_vertices`.
    const bool even = plane_count % 2 == 0;
    Vec3* source = even ? out_vertices : scratch;
    Vec3* destination = even ? scratch : out_vertices;
    usize current_count = count;
    for (usize i = 0; i < count; ++i) {
        source[i] = vertices[i];
    }

    for (usize p = 0; p < plane_count && current_count != 0; ++p) {
        const Expected<usize, Error> clipped =
            clip_polygon_by_plane(source, current_count, planes[p], destination, buffer_capacity);
        if (!clipped) {
            return clipped;
        }
        current_count = *clipped;
        Vec3* const previous_source = source;
        source = destination;
        destination = previous_source;
    }

    // The loop leaves the result in `source`. The parity above arranges for that to be
    // `out_vertices` in the ordinary case; an early exit on an empty polygon can leave it in the
    // scratch buffer instead, so the copy is not dead code.
    if (source != out_vertices) {
        return copy_polygon(source, current_count, out_vertices, buffer_capacity);
    }
    return current_count;
}

// --- AtlasPacker
// -----------------------------------------------------------------------------------

void AtlasPacker::reset(IVec2 size) {
    size_ = size;
    used_area_ = 0;
    skyline_.clear();
    if (size.x > 0 && size.y > 0) {
        skyline_.push_back(SkylineNode{0, 0, size.x});
    }
}

i32 AtlasPacker::fit(usize index, i32 width, i32 height) const noexcept {
    const i32 x = skyline_[index].x;
    if (x + width > size_.x) {
        return -1;
    }
    // The rectangle sits on top of the highest skyline segment it spans, so walk the segments it
    // covers and take the maximum y.
    i32 remaining = width;
    i32 y = skyline_[index].y;
    usize at = index;
    while (remaining > 0) {
        if (at >= skyline_.size()) {
            return -1;
        }
        y = math::max(y, skyline_[at].y);
        if (y + height > size_.y) {
            return -1;
        }
        remaining -= skyline_[at].width;
        ++at;
    }
    return y;
}

void AtlasPacker::add_level(usize index, const IRect& placed) {
    const SkylineNode node{placed.position.x, placed.position.y + placed.size.y, placed.size.x};
    skyline_.insert(skyline_.begin() + static_cast<isize>(index), node);

    // Trim or drop the segments the new one covers.
    for (usize i = index + 1; i < skyline_.size();) {
        const i32 node_right = skyline_[i - 1].x + skyline_[i - 1].width;
        if (skyline_[i].x >= node_right) {
            break;
        }
        const i32 shrink = node_right - skyline_[i].x;
        skyline_[i].x += shrink;
        skyline_[i].width -= shrink;
        if (skyline_[i].width > 0) {
            break;
        }
        skyline_.erase(skyline_.begin() + static_cast<isize>(i));
    }

    // Merge neighbours at the same height, so the skyline does not fragment into a segment per
    // insertion and the search stays proportional to the *distinct heights* rather than to the
    // number of rectangles packed.
    for (usize i = 0; i + 1 < skyline_.size();) {
        if (skyline_[i].y == skyline_[i + 1].y) {
            skyline_[i].width += skyline_[i + 1].width;
            skyline_.erase(skyline_.begin() + static_cast<isize>(i + 1));
        } else {
            ++i;
        }
    }
}

Expected<IRect, Error> AtlasPacker::pack(IVec2 size) {
    if (size.x <= 0 || size.y <= 0) {
        return cy::fail(ErrorCode::InvalidArgument, "AtlasPacker::pack(): a non-positive size");
    }
    if (skyline_.empty()) {
        return cy::fail(ErrorCode::Unavailable, "AtlasPacker::pack(): reset() has not been called");
    }

    // Bottom-left: the lowest y that fits, and among equals the leftmost x.
    i32 best_y = -1;
    i32 best_x = 0;
    usize best_index = 0;
    for (usize i = 0; i < skyline_.size(); ++i) {
        const i32 y = fit(i, size.x, size.y);
        if (y < 0) {
            continue;
        }
        if (best_y < 0 || y < best_y || (y == best_y && skyline_[i].x < best_x)) {
            best_y = y;
            best_x = skyline_[i].x;
            best_index = i;
        }
    }
    if (best_y < 0) {
        return cy::fail(ErrorCode::OutOfRange, "AtlasPacker::pack(): the rectangle does not fit");
    }

    const IRect placed{IVec2{best_x, best_y}, size};
    add_level(best_index, placed);
    used_area_ += placed.area();
    return placed;
}

f32 AtlasPacker::occupancy() const noexcept {
    const i64 total = static_cast<i64>(size_.x) * static_cast<i64>(size_.y);
    if (total <= 0) {
        return 0.0f;
    }
    return static_cast<f32>(static_cast<f64>(used_area_) / static_cast<f64>(total));
}

// --- Mesh utilities
// -----------------------------------------------------------------------------------

namespace {

/// The lattice cell a coordinate falls in, clamped into the 32-bit range before the cast.
///
/// A very small tolerance over a mesh far from the origin overflows an `i32`, and the conversion
/// would then be undefined rather than merely wrong. The clamp turns it into two distant vertices
/// landing in the same saturated cell, which the distance test rejects anyway.
[[nodiscard]] i32 weld_cell_axis(f32 value, f32 inv_cell) noexcept {
    const f64 scaled = std::floor(static_cast<f64>(value) * static_cast<f64>(inv_cell));
    return static_cast<i32>(math::clamp(scaled, -2.0e9, 2.0e9));
}

/// Fold a lattice cell into a 64-bit key: three 21-bit fields, exact for the range a mesh occupies.
[[nodiscard]] u64 weld_cell_key(IVec3 cell) noexcept {
    const u64 x = static_cast<u64>(static_cast<u32>(cell.x)) & 0x1FFFFFull;
    const u64 y = static_cast<u64>(static_cast<u32>(cell.y)) & 0x1FFFFFull;
    const u64 z = static_cast<u64>(static_cast<u32>(cell.z)) & 0x1FFFFFull;
    return (x << 42) | (y << 21) | z;
}

/// The index of an already-emitted vertex within `tolerance` of `p`, or `kNoWeldMatch`.
///
/// Two vertices within tolerance are in the same lattice cell or in one adjacent to it, so the
/// twenty-seven-cell neighbourhood is the complete candidate set — which is what keeps the welder
/// O(n) while still respecting the tolerance in every direction rather than only along the axes.
[[nodiscard]] u32 weld_find_match(const std::unordered_map<u64, std::vector<u32>>& grid, IVec3 cell,
                                  Vec3 p, const Vec3* unique, f32 tolerance_sq) noexcept {
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dy = -1; dy <= 1; ++dy) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                const auto it =
                    grid.find(weld_cell_key(IVec3{cell.x + dx, cell.y + dy, cell.z + dz}));
                if (it == grid.end()) {
                    continue;
                }
                for (const u32 candidate : it->second) {
                    if (distance_squared(unique[candidate], p) <= tolerance_sq) {
                        return candidate;
                    }
                }
            }
        }
    }
    return kNoWeldMatch;
}

}  // namespace

Expected<usize, Error> weld_vertices(const Vec3* positions, usize count, f32 tolerance,
                                     u32* out_remap, Vec3* out_unique) noexcept {
    if (count == 0) {
        return usize{0};
    }
    if (positions == nullptr || out_remap == nullptr || out_unique == nullptr) {
        return cy::fail(ErrorCode::InvalidArgument, "weld_vertices(): a null array");
    }
    if (tolerance <= 0.0f) {
        return cy::fail(ErrorCode::InvalidArgument, "weld_vertices(): tolerance must be positive");
    }

    // A lattice of cells one tolerance wide, so that the candidate set for any vertex is the
    // twenty-seven cells around it rather than the whole mesh.
    const f32 inv_cell = 1.0f / tolerance;
    const f32 tolerance_sq = tolerance * tolerance;
    std::unordered_map<u64, std::vector<u32>> grid;
    grid.reserve(count);
    usize unique_count = 0;

    for (usize i = 0; i < count; ++i) {
        const Vec3 p = positions[i];
        const IVec3 cell{weld_cell_axis(p.x, inv_cell), weld_cell_axis(p.y, inv_cell),
                         weld_cell_axis(p.z, inv_cell)};
        u32 match = weld_find_match(grid, cell, p, out_unique, tolerance_sq);
        if (match == kNoWeldMatch) {
            match = static_cast<u32>(unique_count);
            out_unique[unique_count] = p;
            ++unique_count;
            grid[weld_cell_key(cell)].push_back(match);
        }
        out_remap[i] = match;
    }
    return unique_count;
}

namespace {

/// One triangle's contribution to the tangent frame of its three vertices.
///
/// The per-triangle vectors are left un-normalised on purpose: their magnitude is proportional to
/// the triangle's size, so accumulating them area-weights a shared vertex, which is what a shared
/// vertex wants.
void accumulate_triangle_tangents(const Vec3* positions, const Vec2* uvs, u32 i0, u32 i1, u32 i2,
                                  std::vector<Vec3>& tangents,
                                  std::vector<Vec3>& bitangents) noexcept {
    const Vec3 edge1 = positions[i1] - positions[i0];
    const Vec3 edge2 = positions[i2] - positions[i0];
    const Vec2 delta_uv1 = uvs[i1] - uvs[i0];
    const Vec2 delta_uv2 = uvs[i2] - uvs[i0];

    const f32 determinant = delta_uv1.x * delta_uv2.y - delta_uv2.x * delta_uv1.y;
    if (std::fabs(determinant) < 1e-12f) {
        // Degenerate UVs: this triangle says nothing about the tangent frame. Skipping it is right;
        // contributing a zero would pull its vertices' accumulated tangents toward nothing.
        return;
    }
    const f32 inv = 1.0f / determinant;
    const Vec3 tangent = (edge1 * delta_uv2.y - edge2 * delta_uv1.y) * inv;
    const Vec3 bitangent = (edge2 * delta_uv1.x - edge1 * delta_uv2.x) * inv;

    for (const u32 index : {i0, i1, i2}) {
        tangents[index] += tangent;
        bitangents[index] += bitangent;
    }
}

/// Orthonormalise one accumulated tangent against its normal and pack the handedness into w.
[[nodiscard]] Vec4 finish_tangent(Vec3 normal, Vec3 accumulated, Vec3 bitangent) noexcept {
    // Gram-Schmidt: remove the part of the accumulated tangent that lies along the normal, so the
    // frame is orthogonal even though the accumulation was not.
    Vec3 tangent = accumulated - normal * dot(normal, accumulated);
    if (length_squared(tangent) < math::kSmallLength) {
        // No usable tangent. An arbitrary perpendicular is the honest answer; a zero vector would
        // become a NaN in the shader's normalise and a black pixel nobody can trace back here.
        tangent = any_perpendicular(normalized_or(normal, kAxisUp));
    } else {
        tangent = normalize(tangent);
    }
    // Handedness: +1 when the bitangent follows the right-hand rule from normal and tangent, -1
    // when the UVs are mirrored. This is the fourth component every modern vertex format stores
    // instead of a full bitangent.
    const f32 handedness = dot(cross(normal, tangent), bitangent) < 0.0f ? -1.0f : 1.0f;
    return Vec4{tangent.x, tangent.y, tangent.z, handedness};
}

}  // namespace

Expected<void, Error> generate_tangents(const Vec3* positions, const Vec3* normals, const Vec2* uvs,
                                        usize vertex_count, const u32* indices, usize index_count,
                                        Vec4* out_tangents) noexcept {
    if (vertex_count == 0 || index_count == 0) {
        return {};
    }
    if (positions == nullptr || normals == nullptr || uvs == nullptr || indices == nullptr ||
        out_tangents == nullptr) {
        return cy::fail(ErrorCode::InvalidArgument, "generate_tangents(): a null array");
    }
    if (index_count % 3 != 0) {
        return cy::fail(ErrorCode::InvalidArgument,
                        "generate_tangents(): the index count is not a multiple of three");
    }

    std::vector<Vec3> tangents(vertex_count, Vec3{});
    std::vector<Vec3> bitangents(vertex_count, Vec3{});

    for (usize i = 0; i + 2 < index_count; i += 3) {
        const u32 i0 = indices[i];
        const u32 i1 = indices[i + 1];
        const u32 i2 = indices[i + 2];
        if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) {
            return cy::fail(ErrorCode::OutOfRange,
                            "generate_tangents(): an index is outside the vertex array");
        }
        accumulate_triangle_tangents(positions, uvs, i0, i1, i2, tangents, bitangents);
    }

    for (usize i = 0; i < vertex_count; ++i) {
        out_tangents[i] = finish_tangent(normals[i], tangents[i], bitangents[i]);
    }
    return {};
}

}  // namespace cy::geom
