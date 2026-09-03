// The array-wide operations. Task 3.1.3. See include/cy/core/math/batch.h.
//
// Each operation is written once as a template over a backend tag and instantiated twice: for
// `simd::ReferenceOps` (the always-compiled scalar reference) and for `simd::ActiveOps` (this
// build's SIMD backend, which is the reference again in a build with none). Two instantiations of
// one template is what makes the two paths perform the same operations in the same order, and it is
// why the bit-identity test in tests/test_simd.cpp is a check on the compiler and the intrinsics
// rather than on whether two hand-written loops happen to still agree.

#include <cy/core/math/batch.h>

#include <cy/core/base/assert.h>

namespace cy::math {
namespace {

template <class Ops>
void transform_points_impl(const Mat4& m, const Vec3* in, Vec3* out, usize count) noexcept {
    using Vec = typename Ops::Vec;

    // The four columns are loaded once and reused for every point, which is the whole reason the
    // batch form is faster: a per-point call reloads them and cannot know they are invariant.
    const Vec c0 = Ops::load(&m.columns[0].x);
    const Vec c1 = Ops::load(&m.columns[1].x);
    const Vec c2 = Ops::load(&m.columns[2].x);
    const Vec c3 = Ops::load(&m.columns[3].x);

    for (usize i = 0; i < count; ++i) {
        // Read the whole point before writing anything, so `in == out` is safe.
        const Vec x = Ops::splat(in[i].x);
        const Vec y = Ops::splat(in[i].y);
        const Vec z = Ops::splat(in[i].z);

        // Accumulated back to front: ((c2·z + c3) + c1·y) + c0·x. The order is arbitrary and it is
        // fixed — floating-point addition is not associative, so the reference and the SIMD path
        // must accumulate identically or they are not comparable.
        Vec r = Ops::madd(c2, z, c3);
        r = Ops::madd(c1, y, r);
        r = Ops::madd(c0, x, r);

        f32 lanes[4];
        Ops::store(lanes, r);
        out[i] = Vec3{lanes[0], lanes[1], lanes[2]};
    }
}

template <class Ops>
void transform_directions_impl(const Mat4& m, const Vec3* in, Vec3* out, usize count) noexcept {
    using Vec = typename Ops::Vec;

    const Vec c0 = Ops::load(&m.columns[0].x);
    const Vec c1 = Ops::load(&m.columns[1].x);
    const Vec c2 = Ops::load(&m.columns[2].x);

    for (usize i = 0; i < count; ++i) {
        const Vec x = Ops::splat(in[i].x);
        const Vec y = Ops::splat(in[i].y);
        const Vec z = Ops::splat(in[i].z);

        // The translation column is absent rather than multiplied by a zero w: a direction is not
        // a point with w = 0, it is a point with no translation, and skipping the term is both
        // faster and exact where a multiply by zero would still propagate an infinity.
        Vec r = Ops::mul(c2, z);
        r = Ops::madd(c1, y, r);
        r = Ops::madd(c0, x, r);

        f32 lanes[4];
        Ops::store(lanes, r);
        out[i] = Vec3{lanes[0], lanes[1], lanes[2]};
    }
}

template <class Ops>
void cull_aabbs_impl(const Frustum& frustum, const Aabb* boxes, u8* visible, usize count) noexcept {
    using Vec = typename Ops::Vec;

    // A plane is (nx, ny, nz, d) and a corner is (cx, cy, cz, 1), so the signed distance is one
    // four-wide multiply and a horizontal sum. Packing the plane constant into the fourth lane is
    // what turns "dot three, then add" into a single operation.
    Vec planes[Frustum::kCount];
    for (u32 p = 0; p < Frustum::kCount; ++p) {
        const Plane& plane = frustum.planes[p];
        planes[p] = Ops::set(plane.normal.x, plane.normal.y, plane.normal.z, plane.d);
    }

    for (usize i = 0; i < count; ++i) {
        const Aabb& box = boxes[i];
        if (box.is_empty()) {
            visible[i] = 0;
            continue;
        }
        u8 inside = 1;
        for (u32 p = 0; p < Frustum::kCount; ++p) {
            // Only the corner furthest along the plane normal, selected by the precomputed sign
            // mask. See Frustum in shapes.h for why one corner suffices and why the test is
            // conservative.
            const Vec3 corner = box.corner(frustum.positive_corner[p]);
            const Vec c = Ops::set(corner.x, corner.y, corner.z, 1.0f);
            if (Ops::hsum(Ops::mul(planes[p], c)) < 0.0f) {
                inside = 0;
                break;
            }
        }
        visible[i] = inside;
    }
}

}  // namespace

void transform_points_reference(const Mat4& m, const Vec3* in, Vec3* out, usize count) noexcept {
    CY_ASSERT_MSG(count == 0 || (in != nullptr && out != nullptr),
                  "transform_points(): a null array with a non-zero count");
    transform_points_impl<simd::ReferenceOps>(m, in, out, count);
}

void transform_points_simd(const Mat4& m, const Vec3* in, Vec3* out, usize count) noexcept {
    CY_ASSERT_MSG(count == 0 || (in != nullptr && out != nullptr),
                  "transform_points(): a null array with a non-zero count");
    transform_points_impl<simd::ActiveOps>(m, in, out, count);
}

void transform_points(const Mat4& m, const Vec3* in, Vec3* out, usize count) noexcept {
    transform_points_simd(m, in, out, count);
}

void transform_directions_reference(const Mat4& m, const Vec3* in, Vec3* out,
                                    usize count) noexcept {
    CY_ASSERT_MSG(count == 0 || (in != nullptr && out != nullptr),
                  "transform_directions(): a null array with a non-zero count");
    transform_directions_impl<simd::ReferenceOps>(m, in, out, count);
}

void transform_directions_simd(const Mat4& m, const Vec3* in, Vec3* out, usize count) noexcept {
    CY_ASSERT_MSG(count == 0 || (in != nullptr && out != nullptr),
                  "transform_directions(): a null array with a non-zero count");
    transform_directions_impl<simd::ActiveOps>(m, in, out, count);
}

void transform_directions(const Mat4& m, const Vec3* in, Vec3* out, usize count) noexcept {
    transform_directions_simd(m, in, out, count);
}

void cull_aabbs_reference(const Frustum& frustum, const Aabb* boxes, u8* visible,
                          usize count) noexcept {
    CY_ASSERT_MSG(count == 0 || (boxes != nullptr && visible != nullptr),
                  "cull_aabbs(): a null array with a non-zero count");
    cull_aabbs_impl<simd::ReferenceOps>(frustum, boxes, visible, count);
}

void cull_aabbs_simd(const Frustum& frustum, const Aabb* boxes, u8* visible, usize count) noexcept {
    CY_ASSERT_MSG(count == 0 || (boxes != nullptr && visible != nullptr),
                  "cull_aabbs(): a null array with a non-zero count");
    cull_aabbs_impl<simd::ActiveOps>(frustum, boxes, visible, count);
}

void cull_aabbs(const Frustum& frustum, const Aabb* boxes, u8* visible, usize count) noexcept {
    cull_aabbs_simd(frustum, boxes, visible, count);
}

usize compact_visible(const u8* visible, usize count, u32* out_indices) noexcept {
    CY_ASSERT_MSG(count == 0 || (visible != nullptr && out_indices != nullptr),
                  "compact_visible(): a null array with a non-zero count");
    usize written = 0;
    for (usize i = 0; i < count; ++i) {
        if (visible[i] != 0) {
            out_indices[written++] = static_cast<u32>(i);
        }
    }
    return written;
}

}  // namespace cy::math
