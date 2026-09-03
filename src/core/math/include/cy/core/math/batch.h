#pragma once
// The array-wide operations. Task 3.1.3.
//
// `core-math` — "SIMD strategy": **batch operations (transform arrays of points, cull arrays of
// AABBs) are provided as explicit array-wide functions so vectorisation is deliberate rather than
// hoped for.** A loop over `transform_point()` may or may not vectorise depending on the compiler,
// the inlining decisions above it and whether anything in the loop body aliases; a call to
// `transform_points()` vectorises because it was written to.
//
// Every operation appears three times:
//
//   * `_reference` — the scalar reference (simd.h). Always compiled, on every platform.
//   * `_simd`      — the build's SIMD backend. In a build with no SIMD it is the reference, so a
//                    caller never has to `#ifdef` and a test never has to skip.
//   * unsuffixed   — what production code calls; it is `_simd`.
//
// The three exist so that tests/test_simd.cpp can run the same input through both paths in one
// process and compare. They are not a public menu: engine code calls the unsuffixed form.
//
// BIT-IDENTITY IS BY CONSTRUCTION, AND THEN CHECKED. Each pair is two instantiations of one
// template over the backend tag, so both perform the same operations in the same association order
// — floating-point addition is not associative, and a "obviously equivalent" reordering is a
// different answer. src/CMakeLists.txt compiles this module with floating-point contraction
// disabled so that the compiler cannot fuse a multiply and an add in one path and not the other.
// The test asserts the result rather than trusting the construction, because the construction is
// exactly the kind of thing a later refactor breaks quietly.

#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/simd.h>
#include <cy/core/math/vec.h>

namespace cy::math {

/// Transform `count` positions by `m`, writing to `out`. `in` and `out` may be the same array; they
/// must not otherwise overlap.
///
/// Affine only — w is taken as 1 and there is no perspective divide. Projecting an array of points
/// is a different operation with a different cost, and conflating them would make the common case
/// pay for the rare one.
void transform_points(const Mat4& m, const Vec3* in, Vec3* out, usize count) noexcept;
void transform_points_reference(const Mat4& m, const Vec3* in, Vec3* out, usize count) noexcept;
void transform_points_simd(const Mat4& m, const Vec3* in, Vec3* out, usize count) noexcept;

/// Transform `count` directions by `m`: w is 0, so the translation column does not apply.
void transform_directions(const Mat4& m, const Vec3* in, Vec3* out, usize count) noexcept;
void transform_directions_reference(const Mat4& m, const Vec3* in, Vec3* out, usize count) noexcept;
void transform_directions_simd(const Mat4& m, const Vec3* in, Vec3* out, usize count) noexcept;

/// Test `count` boxes against `frustum`, writing 1 or 0 per box into `visible`.
///
/// Conservative in the direction `Frustum` documents: a box marked invisible is certainly outside,
/// a box marked visible may be outside. One byte per box rather than a bitset because the consumer
/// is a compaction pass that reads them in order, and a byte array is what that pass wants.
void cull_aabbs(const Frustum& frustum, const Aabb* boxes, u8* visible, usize count) noexcept;
void cull_aabbs_reference(const Frustum& frustum, const Aabb* boxes, u8* visible,
                          usize count) noexcept;
void cull_aabbs_simd(const Frustum& frustum, const Aabb* boxes, u8* visible, usize count) noexcept;

/// Turn the byte array `cull_aabbs` produced into the indices of the surviving boxes, and return
/// how many there were. `out_indices` must have room for `count`.
///
/// Separate from the cull because the cull is the part that vectorises and the compaction is the
/// part that does not; fusing them would make the vectorised loop carry a data-dependent store.
[[nodiscard]] usize compact_visible(const u8* visible, usize count, u32* out_indices) noexcept;

}  // namespace cy::math
