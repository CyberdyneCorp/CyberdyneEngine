#pragma once
// The two analytic scenes both occlusion suites read: an open floor, and the same floor meeting a
// wall — an inner corner. Depth and normals are computed per pixel by intersecting the pixel's ray
// with the planes, so what the horizon search is given is exactly what a prepass would write for
// that geometry, and neither suite depends on a rasteriser to say where the corner is.

#include <cy/core/math/projection.h>
#include <cy/rendering/occlusion/gtao.h>

#include <cmath>
#include <vector>

namespace cy::occlusion_test {

inline constexpr u32 kSceneWidth = 160;
inline constexpr u32 kSceneHeight = 120;
inline constexpr f32 kFloorHeight = -1.0F;
inline constexpr f32 kWallDistance = 4.0F;
inline constexpr f32 kNear = 0.1F;
inline constexpr f32 kFar = 100.0F;

struct CornerScene {
    rendering::occlusion::GtaoView view;
    std::vector<f32> depth;
    std::vector<Vec2> normals;
    /// Which plane a pixel sees: 0 sky, 1 floor, 2 wall.
    std::vector<u8> surface;
    /// Distance from the pixel's surface point to the line where the floor meets the wall, in
    /// metres. Infinite without a wall.
    std::vector<f32> corner_distance;
};

/// `wall` false is the open plane. The host suite renders it small, for the unit budget; the device
/// suite at `kSceneWidth` by `kSceneHeight`.
inline CornerScene make_corner_scene(bool wall, u32 width = kSceneWidth,
                                     u32 height = kSceneHeight) {
    CornerScene scene;
    scene.view.width = width;
    scene.view.height = height;
    scene.view.projection = perspective_reversed_z(
        1.0F, static_cast<f32>(width) / static_cast<f32>(height), kNear, kFar);
    scene.view.relative_to_view = Mat4::identity();
    const usize pixels = static_cast<usize>(width) * height;
    scene.depth.assign(pixels, 0.0F);
    scene.normals.assign(pixels, Vec2{0.5F, 0.5F});
    scene.surface.assign(pixels, 0);
    scene.corner_distance.assign(pixels, INFINITY);

    const Mat4& projection = scene.view.projection;
    const f32 p11 = projection.columns[1].y;
    const f32 m22 = projection.columns[2].z;
    const f32 m32 = projection.columns[3].z;
    const Vec2 up = rendering::occlusion::encode_octahedral(Vec3{0.0F, 1.0F, 0.0F});
    const Vec2 towards = rendering::occlusion::encode_octahedral(Vec3{0.0F, 0.0F, 1.0F});
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const f32 ndc_y =
                1.0F - (((static_cast<f32>(y) + 0.5F) / static_cast<f32>(height)) * 2.0F);
            // The ray through the pixel, scaled to unit view depth. Only its height decides which
            // plane it meets: the floor and the wall are both infinite across x.
            const f32 ray_y = ndc_y / p11;
            f32 distance = INFINITY;
            u8 hit = 0;
            if (ray_y < 0.0F) {
                distance = kFloorHeight / ray_y;
                hit = 1;
            }
            if (wall && kWallDistance < distance) {
                distance = kWallDistance;
                hit = 2;
            }
            if (hit == 0 || distance >= kFar) {
                continue;
            }
            const usize index = (static_cast<usize>(y) * width) + x;
            scene.depth[index] = (m32 - (m22 * distance)) / distance;
            scene.normals[index] = hit == 1 ? up : towards;
            scene.surface[index] = hit;
            if (wall) {
                const f32 point_y = ray_y * distance;
                const f32 point_z = -distance;
                const f32 dy = point_y - kFloorHeight;
                const f32 dz = point_z + kWallDistance;
                scene.corner_distance[index] = std::sqrt((dy * dy) + (dz * dz));
            }
        }
    }
    return scene;
}

}  // namespace cy::occlusion_test
