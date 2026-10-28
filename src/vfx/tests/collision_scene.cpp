// SPDX-License-Identifier: MIT
// The stairs, and a camera's depth buffer of them. See collision_scene.h.

#include "collision_scene.h"

#include <cy/core/math/projection.h>

#include <algorithm>
#include <cmath>

namespace cy::vfx_test {

void step_box(u32 index, Vec3& centre, Vec3& half_extents) noexcept {
    const f32 height = static_cast<f32>(index + 1U) * kRise;
    centre = Vec3{0.0F, height * 0.5F, (static_cast<f32>(index) + 0.5F) * kTread};
    half_extents = Vec3{kStairWidth * 0.5F, height * 0.5F, kTread * 0.5F};
}

f32 box_distance(Vec3 point, Vec3 centre, Vec3 half_extents) noexcept {
    const Vec3 q{std::fabs(point.x - centre.x) - half_extents.x,
                 std::fabs(point.y - centre.y) - half_extents.y,
                 std::fabs(point.z - centre.z) - half_extents.z};
    const Vec3 outside{std::max(q.x, 0.0F), std::max(q.y, 0.0F), std::max(q.z, 0.0F)};
    const f32 inside = std::min(std::max({q.x, q.y, q.z}), 0.0F);
    return std::sqrt((outside.x * outside.x) + (outside.y * outside.y) + (outside.z * outside.z)) +
           inside;
}

f32 StairsField::distance(Vec3 point) const noexcept {
    ++samples_;
    f32 nearest = floor_ ? point.y : 1.0e6F;
    for (u32 index = 0; index < kSteps; ++index) {
        Vec3 centre;
        Vec3 half_extents;
        step_box(index, centre, half_extents);
        nearest = std::min(nearest, box_distance(point, centre, half_extents));
    }
    return nearest;
}

namespace {

/// The camera both renderers share, and an empty buffer of its size.
[[nodiscard]] DepthScene camera(Vec3 eye, Vec3 target, u32 width, u32 height, f32 fov_y) noexcept {
    DepthScene scene;
    const Mat4 projection = perspective_reversed_z(
        fov_y, static_cast<f32>(width) / static_cast<f32>(height), 0.05F, 200.0F);
    const Mat4 view = look_at(eye, target);
    scene.view.view_projection = projection * view;
    scene.view.inverse_view_projection =
        inverse(scene.view.view_projection).value_or(Mat4::identity());
    scene.view.camera = eye;
    scene.view.width = width;
    scene.view.height = height;
    scene.depths.assign(static_cast<usize>(width) * height, 0.0F);
    return scene;
}

/// Texel `(x, y)`'s ray direction, by the convention a contact reads the buffer with.
[[nodiscard]] Vec3 texel_ray(const DepthScene& scene, Vec3 eye, u32 x, u32 y) noexcept {
    const f32 u = (static_cast<f32>(x) + 0.5F) / static_cast<f32>(scene.view.width);
    const f32 v = (static_cast<f32>(y) + 0.5F) / static_cast<f32>(scene.view.height);
    const Vec4 far_point =
        scene.view.inverse_view_projection * Vec4{(u * 2.0F) - 1.0F, 1.0F - (v * 2.0F), 0.5F, 1.0F};
    const Vec3 through{far_point.x / far_point.w, far_point.y / far_point.w,
                       far_point.z / far_point.w};
    return normalize(through - eye);
}

[[nodiscard]] f32 device_depth(const DepthScene& scene, Vec3 point) noexcept {
    const Vec4 clip = scene.view.view_projection * Vec4{point.x, point.y, point.z, 1.0F};
    return clip.z / clip.w;
}

}  // namespace

DepthScene render_floor_depth(Vec3 eye, Vec3 target, u32 width, u32 height, f32 fov_y) noexcept {
    DepthScene scene = camera(eye, target, width, height, fov_y);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const Vec3 direction = texel_ray(scene, eye, x, y);
            if (direction.y < -1.0e-4F) {
                const f32 t = -eye.y / direction.y;
                scene.depths[(static_cast<usize>(y) * width) + x] =
                    device_depth(scene, eye + (direction * t));
            }
        }
    }
    scene.view.depths = Span<const f32>(scene.depths.data(), scene.depths.size());
    return scene;
}

DepthScene render_depth(const vfx::CollisionField& field, Vec3 eye, Vec3 target, u32 width,
                        u32 height, f32 fov_y) noexcept {
    DepthScene scene = camera(eye, target, width, height, fov_y);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            // The texel centre's ray, by the convention a contact reads it with: row zero at the
            // top, `ndc.y = +1`.
            const Vec3 direction = texel_ray(scene, eye, x, y);
            f32 t = 0.0F;
            for (u32 step = 0; step < 256 && t < 100.0F; ++step) {
                const f32 d = field.distance(eye + (direction * t));
                if (d < 1.0e-4F) {
                    scene.depths[(static_cast<usize>(y) * width) + x] =
                        device_depth(scene, eye + (direction * t));
                    break;
                }
                t += d;
            }
        }
    }
    scene.view.depths = Span<const f32>(scene.depths.data(), scene.depths.size());
    return scene;
}

}  // namespace cy::vfx_test
