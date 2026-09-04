// Aabb, Obb and Frustum's out-of-line operations. Tasks 3.1.1 and 3.1.4.
// See include/cy/core/math/shapes.h.

#include <cy/core/math/shapes.h>

#include <algorithm>
#include <cmath>

namespace cy {

Aabb transformed(const Aabb& box, const Mat4& m) noexcept {
    if (box.is_empty()) {
        return Aabb::empty();
    }
    // Arvo's method. The transformed centre is exact; the extent grows by the absolute value of the
    // linear part applied to the original extent, which is the smallest axis-aligned box containing
    // the rotated one.
    const Vec3 center = transform_point(m, box.center());
    const Vec3 extent = box.half_extents();
    const Vec3 c0 = cwise_abs(m.columns[0].xyz());
    const Vec3 c1 = cwise_abs(m.columns[1].xyz());
    const Vec3 c2 = cwise_abs(m.columns[2].xyz());
    const Vec3 new_extent = c0 * extent.x + c1 * extent.y + c2 * extent.z;
    return Aabb{center - new_extent, center + new_extent};
}

Vec3 Obb::corner(u32 index) const noexcept {
    const Vec3 local{(index & 1u) != 0u ? half_extents.x : -half_extents.x,
                     (index & 2u) != 0u ? half_extents.y : -half_extents.y,
                     (index & 4u) != 0u ? half_extents.z : -half_extents.z};
    return center + rotation * local;
}

Aabb Obb::bounds() const noexcept {
    // The same absolute-value trick `transformed()` uses, over the rotation's three axes.
    const Mat3 basis = Mat3::from_quat(rotation);
    const Vec3 extent = cwise_abs(basis.columns[0]) * half_extents.x +
                        cwise_abs(basis.columns[1]) * half_extents.y +
                        cwise_abs(basis.columns[2]) * half_extents.z;
    return Aabb{center - extent, center + extent};
}

bool Obb::contains(Vec3 p) const noexcept {
    // Into the box's own frame, where the test is an axis-aligned one.
    const Vec3 local = conjugate(normalize(rotation)) * (p - center);
    return std::fabs(local.x) <= half_extents.x && std::fabs(local.y) <= half_extents.y &&
           std::fabs(local.z) <= half_extents.z;
}

Frustum Frustum::from_view_projection(const Mat4& view_projection) noexcept {
    // Gribb–Hartmann. A point is inside the clip volume when, in homogeneous clip space,
    //   -w <= x <= w,  -w <= y <= w,  0 <= z <= w
    // and each of those six inequalities is a plane whose coefficients are a sum or difference of
    // two rows of the matrix. The rows are what the caller's world position gets dotted with, which
    // is why extraction is rows and not columns even though the storage is columns.
    const Vec4 r0 = view_projection.row(0);
    const Vec4 r1 = view_projection.row(1);
    const Vec4 r2 = view_projection.row(2);
    const Vec4 r3 = view_projection.row(3);

    const Vec4 rows[kCount] = {
        r3 + r0,  // Left:   x >= -w
        r3 - r0,  // Right:  x <=  w
        r3 + r1,  // Bottom: y >= -w
        r3 - r1,  // Top:    y <=  w
        // THE REVERSED-Z HALF. Depth is 1 at the near plane and 0 at the far plane, so the
        // *geometric* near plane is the clip-space bound z <= w and the far plane is z >= 0. Under
        // a conventional depth mapping these two would be the other way round, and swapping them is
        // an error that produces a frustum that culls nothing near and everything far.
        r3 - r2,  // Near:   z <=  w
        r2,       // Far:    z >=  0
    };

    Frustum out;
    for (u32 i = 0; i < kCount; ++i) {
        // Normalised, because the sphere test compares a signed distance against a radius and that
        // is only a distance when the normal is unit-length.
        const Vec3 normal{rows[i].x, rows[i].y, rows[i].z};
        const f32 len = length(normal);
        if (len > math::kSmallLength) {
            out.planes[i] = Plane{normal * (1.0f / len), rows[i].w / len};
        } else {
            // A zero-length normal means that inequality is not a plane at all. THE ENGINE'S
            // DEFAULT PROJECTION IS ONE OF THESE: `perspective_reversed_z_infinite` puts the far
            // plane at infinity, so its row 2 is (0, 0, 0, near) and the far half-space `z >= 0`
            // has no geometry — every point satisfies it.
            //
            // The conservative answer is a plane that rejects nothing, and since
            // `signed_distance(p)` is `dot(normal, p) + d`, that is `d = +infinity` and NOT
            // `-infinity`: a negative infinity makes every signed distance negative and the frustum
            // culls the entire scene. Regression coverage:
            // src/servers/render/tests/test_view.cpp, "the default projection's frustum keeps what
            // is in front of the camera".
            out.planes[i] = Plane{kAxisUp, math::kInfinity};
        }
    }
    out.refresh_corner_masks();
    return out;
}

void Frustum::refresh_corner_masks() noexcept {
    for (u32 i = 0; i < kCount; ++i) {
        const Vec3 n = planes[i].normal;
        // The corner furthest along the normal: max on each axis whose component is non-negative.
        // The bit order matches Aabb::corner(), which is what makes the two agree.
        const u32 mask =
            (n.x >= 0.0f ? 1u : 0u) | (n.y >= 0.0f ? 2u : 0u) | (n.z >= 0.0f ? 4u : 0u);
        positive_corner[i] = static_cast<u8>(mask);
    }
}

bool Frustum::intersects(const Aabb& box) const noexcept {
    if (box.is_empty()) {
        return false;
    }
    for (u32 i = 0; i < kCount; ++i) {
        // Only the corner furthest along the plane normal has to be tested: if even that one is
        // behind the plane, every other corner is too, and the box is wholly outside. This is the
        // whole of the sign-mask optimisation, and it is where the test's conservatism comes from —
        // a box that is behind no single plane may still lie outside the frustum near a corner.
        if (planes[i].signed_distance(box.corner(positive_corner[i])) < 0.0f) {
            return false;
        }
    }
    return true;
}

bool Frustum::intersects(const Sphere& sphere) const noexcept {
    // Spelled as the negation of "behind this plane" rather than as `>= -radius`: the two differ
    // when the distance is NaN, and the outward answer for a degenerate input is that the sphere is
    // kept, which is what the loop this replaced did.
    return std::ranges::all_of(planes, [&](const Plane& plane) noexcept {
        return !(plane.signed_distance(sphere.center) < -sphere.radius);
    });
}

bool Frustum::contains(Vec3 point) const noexcept {
    // Negated for the same reason as `intersects(const Sphere&)` above.
    return std::ranges::all_of(planes, [&](const Plane& plane) noexcept {
        return !(plane.signed_distance(point) < 0.0f);
    });
}

}  // namespace cy
