// SPDX-License-Identifier: MIT
#pragma once
// The analytic scene both contact shadow suites read: a floor, and optionally a box resting on it,
// seen from a camera at the origin looking down -Z. Depth and normals are computed per pixel by
// intersecting the pixel's ray with the geometry, so the trace is given exactly what a prepass
// would write, and the suites can ask the geometry — not a rasteriser — whether a point is in
// contact shadow.

#include <cy/core/math/projection.h>
#include <cy/rendering/contact_shadows/contact.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace cy::contact_test {

inline constexpr u32 kSceneWidth = 384;
inline constexpr u32 kSceneHeight = 256;
inline constexpr f32 kFloorHeight = -1.0F;
inline constexpr f32 kNear = 0.1F;
inline constexpr f32 kFar = 100.0F;

struct Box {
    Vec3 low{0.0F, 0.0F, 0.0F};
    Vec3 high{0.0F, 0.0F, 0.0F};
};

/// The box the contact cases trace against: 0.8 m across, 0.8 m tall, resting on the floor 4 m in
/// front of the camera and a little to its right.
inline constexpr Box kBox{Vec3{-0.2F, kFloorHeight, -4.4F}, Vec3{0.6F, kFloorHeight + 0.8F, -3.6F}};

/// Up, over the camera's left shoulder and from behind the box: the shadow falls to the box's right
/// and toward the camera, onto floor the camera sees.
inline Vec3 contact_light() noexcept {
    const Vec3 to_light{-0.7F, 0.45F, -0.55F};
    return to_light * (1.0F / std::sqrt(dot(to_light, to_light)));
}

struct ContactScene {
    rendering::contact_shadows::ContactShadowView view;
    std::vector<f32> depth;
    std::vector<Vec2> normals;
    /// Which surface a pixel sees: 0 sky, 1 floor, 2 box.
    std::vector<u8> surface;
    /// The surface point, view space (which is camera-relative: the view is the identity).
    std::vector<Vec3> points;
    std::vector<Vec3> surface_normals;
    bool has_box = false;
};

/// The ray's entry parameter into the box and the face normal it enters through; infinity if it
/// misses.
inline f32 ray_box(Vec3 origin, Vec3 direction, const Box& box, Vec3& normal) noexcept {
    f32 near = 0.0F;
    f32 far = INFINITY;
    const f32 o[3] = {origin.x, origin.y, origin.z};
    const f32 d[3] = {direction.x, direction.y, direction.z};
    const f32 low[3] = {box.low.x, box.low.y, box.low.z};
    const f32 high[3] = {box.high.x, box.high.y, box.high.z};
    u32 axis_hit = 0;
    f32 sign = 1.0F;
    for (u32 axis = 0; axis < 3; ++axis) {
        if (std::fabs(d[axis]) < 1.0e-9F) {
            if (o[axis] < low[axis] || o[axis] > high[axis]) {
                return INFINITY;
            }
            continue;
        }
        f32 t0 = (low[axis] - o[axis]) / d[axis];
        f32 t1 = (high[axis] - o[axis]) / d[axis];
        f32 entering = -1.0F;
        if (t0 > t1) {
            std::swap(t0, t1);
            entering = 1.0F;
        }
        if (t0 > near) {
            near = t0;
            axis_hit = axis;
            sign = entering;
        }
        far = std::min(far, t1);
        if (near > far) {
            return INFINITY;
        }
    }
    normal =
        Vec3{axis_hit == 0 ? sign : 0.0F, axis_hit == 1 ? sign : 0.0F, axis_hit == 2 ? sign : 0.0F};
    return near;
}

inline ContactScene make_contact_scene(bool box, u32 width = kSceneWidth,
                                       u32 height = kSceneHeight) {
    ContactScene scene;
    scene.has_box = box;
    scene.view.width = width;
    scene.view.height = height;
    scene.view.projection = perspective_reversed_z(
        0.9F, static_cast<f32>(width) / static_cast<f32>(height), kNear, kFar);
    scene.view.relative_to_view = Mat4::identity();
    scene.view.to_light = contact_light();
    const usize pixels = static_cast<usize>(width) * height;
    scene.depth.assign(pixels, 0.0F);
    scene.normals.assign(pixels, Vec2{0.5F, 0.5F});
    scene.surface.assign(pixels, 0);
    scene.points.assign(pixels, Vec3{0.0F, 0.0F, 0.0F});
    scene.surface_normals.assign(pixels, Vec3{0.0F, 0.0F, 0.0F});

    const Mat4& projection = scene.view.projection;
    const f32 p00 = projection.columns[0].x;
    const f32 p11 = projection.columns[1].y;
    const f32 m22 = projection.columns[2].z;
    const f32 m32 = projection.columns[3].z;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const f32 ndc_x =
                (((static_cast<f32>(x) + 0.5F) / static_cast<f32>(width)) * 2.0F) - 1.0F;
            const f32 ndc_y =
                1.0F - (((static_cast<f32>(y) + 0.5F) / static_cast<f32>(height)) * 2.0F);
            // The ray through the pixel at unit view depth, so its parameter IS the view depth.
            const Vec3 ray{ndc_x / p00, ndc_y / p11, -1.0F};
            f32 distance = INFINITY;
            u8 hit = 0;
            Vec3 normal{0.0F, 1.0F, 0.0F};
            if (ray.y < 0.0F) {
                distance = kFloorHeight / ray.y;
                hit = 1;
            }
            if (box) {
                Vec3 face{0.0F, 0.0F, 0.0F};
                const f32 entry = ray_box(Vec3{0.0F, 0.0F, 0.0F}, ray, kBox, face);
                if (entry < distance) {
                    distance = entry;
                    hit = 2;
                    normal = face;
                }
            }
            if (hit == 0 || distance >= kFar) {
                continue;
            }
            const usize index = (static_cast<usize>(y) * width) + x;
            scene.depth[index] = (m32 - (m22 * distance)) / distance;
            scene.normals[index] = rendering::contact_shadows::encode_octahedral(normal);
            scene.surface[index] = hit;
            scene.points[index] = ray * distance;
            scene.surface_normals[index] = normal;
        }
    }
    return scene;
}

}  // namespace cy::contact_test
