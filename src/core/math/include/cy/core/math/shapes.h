#pragma once
// The bounded shapes: Rect, IRect, Aabb, Obb, Plane, Ray, Sphere and Frustum. Tasks 3.1.1
// and 3.1.4.
//
// `core-math` — "Math types" lists them; "Frustum culling primitives" fixes how `Frustum` works:
// six planes with **precomputed per-plane sign masks**, and an AABB test that selects the extreme
// corner by sign mask. That test is **conservative** — it accepts false positives in exchange for
// being branch-free — and the specification requires that to be documented rather than discovered.
//
// Every shape here is an aggregate with public fields, for the same reasons the vectors are: they
// go into chunk storage, into GPU buffers and through reflection, and an accessor that hides a
// field only makes those three harder.

#include <cy/core/base/assert.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/scalar.h>
#include <cy/core/math/vec.h>

namespace cy {

// --- Rect and IRect
// -----------------------------------------------------------------------------------

/// An axis-aligned 2D rectangle, stored as a position and a size rather than as two corners.
///
/// Position-and-size because that is what a viewport, a scissor, an atlas slot and a UI element all
/// are, and because a size is meaningfully non-negative in a way that "max minus min" is not. `max`
/// is derived.
struct Rect {
    Vec2 position{0.0f, 0.0f};
    Vec2 size{0.0f, 0.0f};

    [[nodiscard]] static constexpr Rect from_min_max(Vec2 min_corner, Vec2 max_corner) noexcept {
        return Rect{min_corner, max_corner - min_corner};
    }

    [[nodiscard]] constexpr Vec2 min() const noexcept { return position; }
    [[nodiscard]] constexpr Vec2 max() const noexcept { return position + size; }
    [[nodiscard]] constexpr Vec2 center() const noexcept { return position + size * 0.5f; }
    [[nodiscard]] constexpr f32 area() const noexcept { return size.x * size.y; }
    [[nodiscard]] constexpr bool is_empty() const noexcept {
        return size.x <= 0.0f || size.y <= 0.0f;
    }

    /// Half-open in both axes: a point on the min edge is inside, one on the max edge is not. That
    /// is what makes a grid of rectangles tile without a point belonging to two of them.
    [[nodiscard]] constexpr bool contains(Vec2 p) const noexcept {
        const Vec2 hi = max();
        return p.x >= position.x && p.y >= position.y && p.x < hi.x && p.y < hi.y;
    }

    [[nodiscard]] constexpr bool intersects(const Rect& other) const noexcept {
        const Vec2 hi = max();
        const Vec2 other_hi = other.max();
        return position.x < other_hi.x && other.position.x < hi.x && position.y < other_hi.y &&
               other.position.y < hi.y;
    }
};

/// The integer rectangle: pixels, texels, atlas slots, scissor boxes.
struct IRect {
    IVec2 position{0, 0};
    IVec2 size{0, 0};

    [[nodiscard]] constexpr IVec2 min() const noexcept { return position; }
    [[nodiscard]] constexpr IVec2 max() const noexcept { return position + size; }
    [[nodiscard]] constexpr i64 area() const noexcept {
        return static_cast<i64>(size.x) * static_cast<i64>(size.y);
    }
    [[nodiscard]] constexpr bool is_empty() const noexcept { return size.x <= 0 || size.y <= 0; }

    [[nodiscard]] constexpr bool contains(IVec2 p) const noexcept {
        const IVec2 hi = max();
        return p.x >= position.x && p.y >= position.y && p.x < hi.x && p.y < hi.y;
    }

    [[nodiscard]] constexpr bool intersects(const IRect& other) const noexcept {
        const IVec2 hi = max();
        const IVec2 other_hi = other.max();
        return position.x < other_hi.x && other.position.x < hi.x && position.y < other_hi.y &&
               other.position.y < hi.y;
    }
};

[[nodiscard]] constexpr bool operator==(const IRect& a, const IRect& b) noexcept {
    return a.position == b.position && a.size == b.size;
}
[[nodiscard]] constexpr bool operator!=(const IRect& a, const IRect& b) noexcept {
    return !(a == b);
}

// --- Aabb
// ---------------------------------------------------------------------------------------------

/// An axis-aligned bounding box, stored as its two extreme corners.
///
/// Min-and-max rather than centre-and-extents because the operations that matter — merging two
/// boxes, growing one by a point, testing overlap — are componentwise min/max on this form and
/// require a conversion on the other. The default value is the **empty** box (min at +inf, max at
/// -inf), which is the identity for `merge` and `grow`: accumulating over zero points leaves it
/// empty rather than leaving a degenerate box at the origin that swallows the world's centre.
struct Aabb {
    Vec3 min{math::kInfinity, math::kInfinity, math::kInfinity};
    Vec3 max{-math::kInfinity, -math::kInfinity, -math::kInfinity};

    [[nodiscard]] static constexpr Aabb empty() noexcept { return Aabb{}; }

    [[nodiscard]] static constexpr Aabb from_min_max(Vec3 lo, Vec3 hi) noexcept {
        return Aabb{lo, hi};
    }

    [[nodiscard]] static constexpr Aabb from_center_extents(Vec3 center,
                                                            Vec3 half_extents) noexcept {
        return Aabb{center - half_extents, center + half_extents};
    }

    [[nodiscard]] static constexpr Aabb from_point(Vec3 p) noexcept { return Aabb{p, p}; }

    [[nodiscard]] constexpr bool is_empty() const noexcept {
        return min.x > max.x || min.y > max.y || min.z > max.z;
    }

    [[nodiscard]] constexpr Vec3 center() const noexcept { return (min + max) * 0.5f; }
    [[nodiscard]] constexpr Vec3 size() const noexcept { return max - min; }
    [[nodiscard]] constexpr Vec3 half_extents() const noexcept { return (max - min) * 0.5f; }

    /// Twice the sum of the three face areas. The surface-area heuristic that drives both BVH
    /// builders is stated in terms of this, and it is proportional to the probability that a
    /// uniformly distributed ray hits the box — which is why SAH works.
    [[nodiscard]] constexpr f32 surface_area() const noexcept {
        if (is_empty()) {
            return 0.0f;
        }
        const Vec3 d = size();
        return 2.0f * ((d.x * d.y) + (d.y * d.z) + (d.z * d.x));
    }

    [[nodiscard]] constexpr f32 volume() const noexcept {
        if (is_empty()) {
            return 0.0f;
        }
        const Vec3 d = size();
        return d.x * d.y * d.z;
    }

    /// Inclusive on both bounds, unlike `Rect::contains`. A bounding volume is a *closed* set: a
    /// point exactly on the surface of a mesh's bounds is inside its bounds.
    [[nodiscard]] constexpr bool contains(Vec3 p) const noexcept {
        return p.x >= min.x && p.y >= min.y && p.z >= min.z && p.x <= max.x && p.y <= max.y &&
               p.z <= max.z;
    }

    [[nodiscard]] constexpr bool contains(const Aabb& other) const noexcept {
        return other.min.x >= min.x && other.min.y >= min.y && other.min.z >= min.z &&
               other.max.x <= max.x && other.max.y <= max.y && other.max.z <= max.z;
    }

    [[nodiscard]] constexpr bool intersects(const Aabb& other) const noexcept {
        return min.x <= other.max.x && other.min.x <= max.x && min.y <= other.max.y &&
               other.min.y <= max.y && min.z <= other.max.z && other.min.z <= max.z;
    }

    /// One of the eight corners, selected bitwise: bit 0 chooses `max.x` over `min.x`, bit 1
    /// `max.y`, bit 2 `max.z`. The same bit order the frustum sign masks use, so the two agree.
    [[nodiscard]] constexpr Vec3 corner(u32 index) const noexcept {
        return Vec3{(index & 1u) != 0u ? max.x : min.x, (index & 2u) != 0u ? max.y : min.y,
                    (index & 4u) != 0u ? max.z : min.z};
    }

    constexpr void grow(Vec3 p) noexcept {
        min = cwise_min(min, p);
        max = cwise_max(max, p);
    }

    constexpr void grow(const Aabb& other) noexcept {
        min = cwise_min(min, other.min);
        max = cwise_max(max, other.max);
    }

    /// Grown outward on every axis by `margin`. This is the "fat AABB" the dynamic BVH inserts, so
    /// that a small movement stays inside the stored bounds and does not restructure the tree.
    [[nodiscard]] constexpr Aabb expanded(f32 margin) const noexcept {
        const Vec3 m{margin, margin, margin};
        return Aabb{min - m, max + m};
    }

    /// The nearest point of the box to `p`; `p` itself when it is inside.
    [[nodiscard]] constexpr Vec3 closest_point(Vec3 p) const noexcept {
        return cwise_min(cwise_max(p, min), max);
    }
};

[[nodiscard]] constexpr Aabb merge(const Aabb& a, const Aabb& b) noexcept {
    return Aabb{cwise_min(a.min, b.min), cwise_max(a.max, b.max)};
}

[[nodiscard]] constexpr Aabb intersection(const Aabb& a, const Aabb& b) noexcept {
    return Aabb{cwise_max(a.min, b.min), cwise_min(a.max, b.max)};
}

/// The AABB of `box` transformed by `m` — the bounds of the transformed box, not the transform of
/// the bounds, which would be a rotated box and no longer axis-aligned.
///
/// Uses the absolute-value trick rather than transforming eight corners: the extent of the result
/// along each axis is `|M| · extent`, which is three dot products instead of eight matrix-vector
/// products. It is conservative in the same sense the frustum test is — the result contains the
/// true bounds and may be slightly larger after a rotation.
[[nodiscard]] Aabb transformed(const Aabb& box, const Mat4& m) noexcept;

[[nodiscard]] constexpr bool operator==(const Aabb& a, const Aabb& b) noexcept {
    return a.min == b.min && a.max == b.max;
}
[[nodiscard]] constexpr bool operator!=(const Aabb& a, const Aabb& b) noexcept {
    return !(a == b);
}

// --- Obb
// ------------------------------------------------------------------------------------------------

/// An oriented bounding box: a centre, a rotation, and half-extents along its own axes.
struct Obb {
    Vec3 center{0.0f, 0.0f, 0.0f};
    Vec3 half_extents{0.0f, 0.0f, 0.0f};
    Quat rotation = Quat::identity();

    [[nodiscard]] Vec3 corner(u32 index) const noexcept;

    /// The axis-aligned bounds that contain this box.
    [[nodiscard]] Aabb bounds() const noexcept;

    [[nodiscard]] bool contains(Vec3 p) const noexcept;
};

// --- Plane
// ------------------------------------------------------------------------------------------------

/// A plane, stored as a unit normal and the signed distance from the origin along it: the point set
/// where `dot(normal, p) + d == 0`.
///
/// `signed_distance` is positive on the side the normal points to. Every plane test in the engine
/// is written in terms of that sign, including the frustum test, so the sign convention is stated
/// once here and never restated.
struct Plane {
    Vec3 normal{0.0f, 1.0f, 0.0f};
    f32 d = 0.0f;

    [[nodiscard]] static Plane from_point_normal(Vec3 point, Vec3 unit_normal) noexcept {
        return Plane{unit_normal, -dot(unit_normal, point)};
    }

    /// The plane through three points, with the normal following the right-hand rule around
    /// a → b → c (counter-clockwise winding faces the normal).
    [[nodiscard]] static Plane from_points(Vec3 a, Vec3 b, Vec3 c) noexcept {
        const Vec3 n = normalize(cross(b - a, c - a));
        return from_point_normal(a, n);
    }

    [[nodiscard]] constexpr f32 signed_distance(Vec3 p) const noexcept {
        return dot(normal, p) + d;
    }

    [[nodiscard]] constexpr Vec3 project(Vec3 p) const noexcept {
        return p - normal * signed_distance(p);
    }

    /// Rescaled so the normal is unit-length. A plane extracted from a projection matrix is not
    /// normalised, and a signed distance from a non-normalised plane is scaled by an unknown
    /// factor — fine for a sign test, wrong for anything that compares against a radius.
    [[nodiscard]] Plane normalized() const noexcept {
        const f32 len = length(normal);
        CY_ASSERT_MSG(len > math::kSmallLength, "Plane::normalized() on a degenerate plane");
        const f32 inv = 1.0f / len;
        return Plane{normal * inv, d * inv};
    }
};

// --- Ray and Sphere
// ---------------------------------------------------------------------------------------

/// A ray: an origin and a direction that callers are expected to keep unit-length. `t` values
/// returned by the intersection routines in geometry.h are distances along it, in metres, which is
/// only true while the direction is normalised.
struct Ray {
    Vec3 origin{0.0f, 0.0f, 0.0f};
    Vec3 direction{0.0f, 0.0f,
                   -1.0f};  // the engine's forward, so a default Ray looks where a camera does

    [[nodiscard]] constexpr Vec3 at(f32 t) const noexcept { return origin + direction * t; }
};

struct Sphere {
    Vec3 center{0.0f, 0.0f, 0.0f};
    f32 radius = 0.0f;

    [[nodiscard]] constexpr bool contains(Vec3 p) const noexcept {
        return distance_squared(p, center) <= radius * radius;
    }

    [[nodiscard]] constexpr bool intersects(const Sphere& other) const noexcept {
        const f32 sum = radius + other.radius;
        return distance_squared(center, other.center) <= sum * sum;
    }

    [[nodiscard]] constexpr Aabb bounds() const noexcept {
        const Vec3 r{radius, radius, radius};
        return Aabb{center - r, center + r};
    }
};

// --- Frustum
// ---------------------------------------------------------------------------------------------

/// The six planes of a view frustum, with the per-plane sign masks the AABB test needs.
///
/// `core-math` — "Frustum culling primitives". Plane normals point **inward**, so a point is inside
/// the frustum when its signed distance to every plane is non-negative, and one negative distance
/// is enough to reject.
///
/// **The AABB test is conservative**: it may report a box as visible that is outside the frustum
/// (the classic case is a large box straddling the region just outside a corner, which lies on the
/// inner side of all six planes individually). It never reports a box as invisible that is visible,
/// which is the direction that matters — a false positive costs a draw call, a false negative is a
/// hole in the image. The specification requires that trade to be documented; this paragraph is it.
struct Frustum {
    /// The plane order is fixed so that a caller may index by name. `Near` and `Far` are named for
    /// the *geometry*, not for the clip-space bound they came from: under reversed Z the near plane
    /// is the `w − z ≥ 0` half-space and the far plane is `z ≥ 0`, which is the opposite of the
    /// conventional-depth assignment and is exactly the kind of thing that is wrong for a week
    /// before anyone notices.
    enum PlaneIndex : u32 { Left = 0, Right, Bottom, Top, Near, Far, kCount };

    Plane planes[kCount];

    /// One byte per plane: bit 0 set when the normal's x is non-negative, bit 1 for y, bit 2 for z.
    /// It selects the AABB corner furthest along the plane normal — `Aabb::corner()` uses the same
    /// bit order — which is the only corner that has to be tested to reject the whole box.
    u8 positive_corner[kCount] = {};

    /// Extract from a view-projection matrix by the Gribb–Hartmann method, adapted to the engine's
    /// [0, 1] reversed-Z clip volume. The planes come out normalised.
    [[nodiscard]] static Frustum from_view_projection(const Mat4& view_projection) noexcept;

    /// Recompute `positive_corner` after the planes have been written directly.
    void refresh_corner_masks() noexcept;

    /// Conservative: false when the box is certainly outside, true when it may intersect.
    [[nodiscard]] bool intersects(const Aabb& box) const noexcept;

    /// Exact for a sphere — a sphere has no corners to be conservative about. False when the sphere
    /// is entirely behind some plane.
    [[nodiscard]] bool intersects(const Sphere& sphere) const noexcept;

    [[nodiscard]] bool contains(Vec3 point) const noexcept;
};

}  // namespace cy
