#pragma once
// Projections, view matrices, and the depth convention as data. Task 3.1.2.
//
// `core-math` — "Depth and projection conventions", stated normatively and asserted numerically in
// tests/test_conventions.cpp:
//
//   * The depth range is **[0, 1]**, not [-1, 1]. That is the Vulkan/D3D/Metal range; there is no
//     OpenGL-style projection in the engine.
//   * Depth is **reversed**: the **near plane maps to 1.0** and the **far plane to 0.0**.
//   * Depth buffers are **cleared to 0.0**, opaque geometry compares **GreaterEqual**, and shadow
//     comparison samplers use **Greater**.
//   * Reversed Z with a floating-point depth buffer is the only supported configuration, so
//     precision behaviour is uniform across backends.
//
// Reversed Z is not a preference. A [0,1] float depth buffer has its exponent resolution bunched
// near 0, and a conventional projection bunches its *depth* resolution near the far plane; putting
// the two together wastes almost the whole format. Reversing the mapping lines the two curves up
// and is worth several orders of magnitude of depth precision at distance, which is what makes the
// infinite far plane below a reasonable default rather than a trick.
//
// WHAT IS NOT HERE. The viewport Y flip that Vulkan's clip space needs is a *viewport* concern and
// belongs to the render backend at M3: a projection matrix that had it baked in would be wrong for
// every other API and would make this file backend-specific. These matrices are pure mathematics.

#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/scalar.h>
#include <cy/core/math/vec.h>

namespace cy {

/// The depth comparison a pipeline is created with.
///
/// Declared here, in math, rather than in the renderer, because it is a *consequence* of the
/// projection convention rather than a rendering option: with the near plane at 1 and the far plane
/// at 0, "closer" means "greater", and a pipeline that says `Less` is not making a different
/// artistic choice, it is wrong. M3's backends map these onto `VkCompareOp` and friends; the
/// engine-side names are what a pipeline description carries.
enum class DepthCompareOp : u32 {
    Never = 0,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always,
};

/// The depth convention, as values a pipeline reads rather than as prose a pipeline author
/// remembers. `core-math` — "Depth test direction": an opaque pipeline's compare op is
/// `GreaterEqual` and its depth clear is 0.0.
struct DepthConvention {
    /// Reversed Z is the only supported configuration.
    static constexpr bool kReversedZ = true;
    /// The clip-space depth range. Never [-1, 1].
    static constexpr f32 kNdcMin = 0.0f;
    static constexpr f32 kNdcMax = 1.0f;
    /// What the near and far planes map to under the reversal.
    static constexpr f32 kNearPlaneDepth = 1.0f;
    static constexpr f32 kFarPlaneDepth = 0.0f;
    /// What a depth attachment is cleared to at the start of a frame: the far plane's value.
    static constexpr f32 kClearValue = 0.0f;
    /// The comparison an opaque pipeline uses, and the one a shadow comparison sampler uses.
    static constexpr DepthCompareOp kOpaque = DepthCompareOp::GreaterEqual;
    static constexpr DepthCompareOp kShadow = DepthCompareOp::Greater;
};

/// Whether `depth_a` is nearer to the camera than `depth_b`, in the engine's depth space.
///
/// Provided so that sorting and occlusion code expresses "nearer" once rather than open-coding a
/// `>` that reads backwards to anyone who has worked in a conventional depth engine.
[[nodiscard]] inline constexpr bool depth_is_nearer(f32 depth_a, f32 depth_b) noexcept {
    return depth_a > depth_b;
}

// --- Projections
// ------------------------------------------------------------------------------------

/// A right-handed perspective projection onto reversed-Z [0, 1] depth.
///
/// The camera looks down its local −Z, so a point in front of it has a negative view-space z.
/// `near_plane` and `far_plane` are positive distances along that view direction.
///
/// The numeric consequence, asserted in tests/test_conventions.cpp: a point at −`near_plane`
/// projects to depth exactly 1, and a point at −`far_plane` to exactly 0.
[[nodiscard]] Mat4 perspective_reversed_z(f32 fov_y_radians, f32 aspect, f32 near_plane,
                                          f32 far_plane) noexcept;

/// The same, with the far plane at infinity.
///
/// `core-math` — "Infinite far plane": reversed Z keeps precision well distributed at distance, and
/// the projection helper exposes this as a supported mode. It is the limit of the finite form as
/// `far_plane → ∞`; nothing is clipped at the back, and depth approaches 0 asymptotically without
/// ever reaching it, so nothing is ever exactly at the far plane's value.
[[nodiscard]] Mat4 perspective_reversed_z_infinite(f32 fov_y_radians, f32 aspect,
                                                   f32 near_plane) noexcept;

/// A right-handed orthographic projection onto reversed-Z [0, 1] depth. Same near/far mapping as
/// the perspective form: near to 1, far to 0.
[[nodiscard]] Mat4 orthographic_reversed_z(f32 left, f32 right, f32 bottom, f32 top, f32 near_plane,
                                           f32 far_plane) noexcept;

/// The view matrix for a camera at `eye` looking toward `target`.
///
/// The camera's local −Z points at the target, so the returned matrix is the inverse of a
/// `Transform` whose `forward()` is `normalize(target - eye)`. A camera at the origin looking down
/// −Z therefore has the identity view matrix, which is the assertion the convention test makes.
[[nodiscard]] Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up = kAxisUp) noexcept;

/// The view matrix for a camera placed by a `Transform`: the inverse of its rotation and
/// translation. Scale is ignored — a scaled camera is not a camera.
[[nodiscard]] Mat4 view_from_transform(const Quat& rotation, Vec3 position) noexcept;

/// Recover the near and far distances a reversed-Z perspective matrix was built with. Returns
/// `kInfinity` for `far` when the matrix came from `perspective_reversed_z_infinite`.
///
/// Exists for the diagnostics path: a depth-precision problem is nearly always a near plane that
/// someone set to 0.001, and reading it back off the matrix that is actually in use is more
/// reliable than reading it off the camera component that was supposed to have produced it.
void perspective_planes(const Mat4& projection, f32& near_plane, f32& far_plane) noexcept;

}  // namespace cy
