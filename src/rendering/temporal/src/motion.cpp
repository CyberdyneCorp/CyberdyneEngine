#include <cy/rendering/temporal/motion.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering {
namespace {

/// Clip space to [0,1]² screen space. `w` comes back so the caller can reject a point behind the
/// camera, which is the whole of `representable`.
[[nodiscard]] bool to_screen(const Mat4& view_projection, Vec3 world, Vec2& out) noexcept {
    const Vec4 clip = view_projection * Vec4{world.x, world.y, world.z, 1.0F};
    if (clip.w <= math::kSmallLength) {
        return false;
    }
    const f32 inverse_w = 1.0F / clip.w;
    out = Vec2{((clip.x * inverse_w) * 0.5F) + 0.5F, ((clip.y * inverse_w) * 0.5F) + 0.5F};
    return true;
}

}  // namespace

SurfaceMotion derive_surface_motion(const SurfaceMotionInputs& inputs) noexcept {
    SurfaceMotion motion;
    Vec2 current;
    Vec2 previous;
    // Both matrices are unjittered, so a static surface under a static camera answers exactly zero
    // however the projection was jittered — see the convention in the header.
    if (!to_screen(inputs.current_view_projection, inputs.current_world, current)) {
        return motion;
    }
    if (!to_screen(inputs.previous_view_projection, inputs.previous_world, previous)) {
        motion.current_uv = current;
        return motion;
    }
    motion.current_uv = current;
    motion.motion = Vec2{previous.x - current.x, previous.y - current.y};
    motion.representable = true;
    return motion;
}

f32 motion_pixels(Vec2 motion, u32 width, u32 height) noexcept {
    const f32 x = motion.x * static_cast<f32>(width);
    const f32 y = motion.y * static_cast<f32>(height);
    return std::sqrt((x * x) + (y * y));
}

}  // namespace cy::rendering
