#pragma once
// Motion vectors, derived from data the renderer already holds. Task 8.3.
//
// `temporal-rendering` — "Motion vectors are derived, not authored". The GPU scene already stores
// current and previous instance transforms and the GPU pose world already stores current and
// previous poses, so a motion vector is a subtraction rather than a thing an artist supplies. That
// is why every moving surface — static, skinned, virtual geometry, mesh particles, world-space UI —
// reprojects correctly with no per-system work: none of them has any work to do.
//
// ================================================================================================
// THE CONVENTION, WRITTEN DOWN ONCE
// ================================================================================================
//
// **A motion vector takes a pixel to where its surface was last frame, in normalised screen space,
// with jitter removed.** So `history_uv = current_uv + motion`, and a static surface under a static
// camera has a motion vector of exactly zero however the projection was jittered.
//
// Both halves of that sentence are decisions and both are load-bearing:
//
//   * The DIRECTION is toward the past, because every consumer of a motion vector is reading
//     history. Storing the forward vector means every consumer negates, and the one that forgets
//     produces a trail that points the wrong way and looks like a reprojection that is twice too
//     strong rather than one that is backwards.
//   * The JITTER IS REMOVED here and not by each consumer. Leaving it in makes a static scene's
//     motion vectors a sub-pixel dither, which reads as camera shake to a neighbourhood clamp and
//     is the classic source of "TAA is soft and I cannot find why".
//
// `temporal-rendering` requires exactly one convention shared by every effect, so both statements
// are enforced by there being one function.

#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>

namespace cy::rendering {

/// Everything the derivation reads for one surface point. Every field is data the renderer already
/// holds; there is no authored input in this structure and that is the requirement.
struct SurfaceMotionInputs {
    /// The point's world position this frame and last frame. For a skinned vertex these come from
    /// the current and previous poses; for a rigid instance, from the two transforms; for
    /// world-space UI, from the two widget transforms. The derivation does not care which.
    Vec3 current_world{0.0F, 0.0F, 0.0F};
    Vec3 previous_world{0.0F, 0.0F, 0.0F};
    /// View-projection matrices WITHOUT jitter. The framework holds the jitter separately for
    /// exactly this reason.
    Mat4 current_view_projection = Mat4::identity();
    Mat4 previous_view_projection = Mat4::identity();
};

struct SurfaceMotion {
    /// `history_uv = current_uv + motion`. Normalised screen space.
    Vec2 motion{0.0F, 0.0F};
    /// Where the point is this frame, in [0,1]² screen space. Handed back because every caller that
    /// wants the motion also wants this and computing it twice is the usual accident.
    Vec2 current_uv{0.0F, 0.0F};
    /// False when the motion cannot be expressed as a screen-space vector at all — the surface was
    /// behind the camera last frame, or is behind it now. "Surfaces whose motion cannot be
    /// represented SHALL be marked so consumers can reject history for them rather than smearing."
    bool representable = false;
};

[[nodiscard]] SurfaceMotion derive_surface_motion(const SurfaceMotionInputs& inputs) noexcept;

/// Where a pixel's history is. The one place the convention's sign is applied.
[[nodiscard]] inline Vec2 reproject(Vec2 current_uv, Vec2 motion) noexcept {
    return Vec2{current_uv.x + motion.x, current_uv.y + motion.y};
}

/// Screen-space speed in pixels per frame, for a target of this size. What a neighbourhood clamp
/// widens on and what `staleness` in other subsystems reads as "motion".
[[nodiscard]] f32 motion_pixels(Vec2 motion, u32 width, u32 height) noexcept;

}  // namespace cy::rendering
