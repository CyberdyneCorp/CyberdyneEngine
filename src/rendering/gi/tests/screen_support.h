// SPDX-License-Identifier: MIT
#pragma once
// A view of a scene of axis-aligned quads, ray cast into the surfaces a per-pixel resolve reads.
// Shared by the host suite (`integration.render_gi_screen`) and the device suite
// (`render.gi_gpu_resolve`), so both resolve the same pixels. Issue #35, stage 4.
//
// The scene is a list of rectangles, each in a plane of constant x, y or z, which is every shape
// the GI rooms are built from: the room's six walls, its divider, and a 10 cm wall's two faces and
// its edges. Each rectangle carries the albedo the GI scene's cards for it have, so a test can
// also write the cards from the same list.

#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/math/vec.h>
#include <cy/rendering/gi/lighting.h>
#include <cy/rendering/gi/scene.h>
#include <cy/rendering/gi/screen_resolve.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace gi_support {

/// A rectangle in the plane `axis = coordinate`, spanning [lo, hi] on the other two axes (in
/// x, y, z order with the plane's axis skipped). Two-sided: a hit's normal faces the ray.
struct AxisQuad {
    cy::u32 axis = 0;
    cy::f32 coordinate = 0.0F;
    cy::f32 lo[2] = {0.0F, 0.0F};
    cy::f32 hi[2] = {0.0F, 0.0F};
    cy::Vec3 albedo{0.6F, 0.6F, 0.6F};
    /// The side the quad's cards face: +1 along its axis, -1 against it, 0 both — a divider seen
    /// from either half of a room. Intersection is two-sided whatever this says.
    cy::f32 facing = 0.0F;
};

[[nodiscard]] inline cy::f32 component(cy::Vec3 v, cy::u32 axis) noexcept {
    if (axis == 0) {
        return v.x;
    }
    return axis == 1 ? v.y : v.z;
}

/// The nearest quad a ray meets, its distance and its normal facing the ray.
[[nodiscard]] inline bool cast(const std::vector<AxisQuad>& quads, cy::Vec3 origin,
                               cy::Vec3 direction, cy::f32& t_out, cy::Vec3& normal_out) {
    bool found = false;
    t_out = 1.0e30F;
    for (const AxisQuad& quad : quads) {
        const cy::f32 d = component(direction, quad.axis);
        if (std::abs(d) < 1.0e-9F) {
            continue;
        }
        const cy::f32 t = (quad.coordinate - component(origin, quad.axis)) / d;
        if (t <= 1.0e-4F || t >= t_out) {
            continue;
        }
        const cy::Vec3 point = origin + (direction * t);
        const cy::u32 a = quad.axis == 0 ? 1U : 0U;
        const cy::u32 b = quad.axis == 2 ? 1U : 2U;
        const cy::f32 u = component(point, a);
        const cy::f32 v = component(point, b);
        if (u < quad.lo[0] || u > quad.hi[0] || v < quad.lo[1] || v > quad.hi[1]) {
            continue;
        }
        t_out = t;
        cy::Vec3 normal{0.0F, 0.0F, 0.0F};
        const cy::f32 facing = d > 0.0F ? -1.0F : 1.0F;
        if (quad.axis == 0) {
            normal.x = facing;
        } else if (quad.axis == 1) {
            normal.y = facing;
        } else {
            normal.z = facing;
        }
        normal_out = normal;
        found = true;
    }
    return found;
}

/// One camera's surfaces.
struct ScreenView {
    cy::u32 width = 0;
    cy::u32 height = 0;
    cy::Vec3 camera{0.0F, 0.0F, 0.0F};
    cy::Mat4 world_to_view = cy::Mat4::identity();
    cy::Mat4 world_to_clip = cy::Mat4::identity();
    /// The projection alone, for the four terms the device unpack reads.
    cy::Mat4 projection = cy::Mat4::identity();
    std::vector<cy::f32> depth;
    std::vector<cy::Vec3> position;
    std::vector<cy::Vec3> normal;
    std::vector<cy::f32> roughness;

    [[nodiscard]] cy::rendering::gi::ScreenSurfaces surfaces() const noexcept {
        cy::rendering::gi::ScreenSurfaces out;
        out.width = width;
        out.height = height;
        out.depth = {depth.data(), depth.size()};
        out.position = {position.data(), position.size()};
        out.normal = {normal.data(), normal.size()};
        out.roughness = {roughness.data(), roughness.size()};
        out.camera = camera;
        return out;
    }
};

/// Ray cast `quads` from `eye` toward `target` through every pixel centre. Rows run top to bottom,
/// as the frame's do; a pixel's ray is the inverse of `world_to_clip` at its centre, so projecting
/// the hit back lands on the pixel it came from. Every surface gets `roughness`.
[[nodiscard]] inline ScreenView render_view(const std::vector<AxisQuad>& quads, cy::Vec3 eye,
                                            cy::Vec3 target, cy::u32 width, cy::u32 height,
                                            cy::f32 roughness = 0.8F, cy::f32 fov_degrees = 70.0F) {
    ScreenView view;
    view.width = width;
    view.height = height;
    view.camera = eye;
    const cy::f32 aspect = static_cast<cy::f32>(width) / static_cast<cy::f32>(height);
    view.projection =
        cy::perspective_reversed_z(fov_degrees * 0.01745329252F, aspect, 0.05F, 200.0F);
    view.world_to_view = cy::look_at(eye, target);
    view.world_to_clip = view.projection * view.world_to_view;
    const cy::Mat4 clip_to_world = cy::inverse(view.world_to_clip).value();
    const cy::Vec3 forward = cy::normalized_or(target - eye, cy::Vec3{0.0F, 0.0F, -1.0F});
    const auto pixels = static_cast<size_t>(width) * height;
    view.depth.assign(pixels, 0.0F);
    view.position.assign(pixels, cy::Vec3{0.0F, 0.0F, 0.0F});
    view.normal.assign(pixels, cy::Vec3{0.0F, 1.0F, 0.0F});
    view.roughness.assign(pixels, roughness);
    for (cy::u32 y = 0; y < height; ++y) {
        for (cy::u32 x = 0; x < width; ++x) {
            const cy::f32 ndc_x =
                (((static_cast<cy::f32>(x) + 0.5F) / static_cast<cy::f32>(width)) * 2.0F) - 1.0F;
            const cy::f32 ndc_y =
                1.0F - (((static_cast<cy::f32>(y) + 0.5F) / static_cast<cy::f32>(height)) * 2.0F);
            const cy::Vec3 far_point =
                cy::project_point(clip_to_world, cy::Vec3{ndc_x, ndc_y, 0.5F});
            const cy::Vec3 direction =
                cy::normalized_or(far_point - eye, cy::Vec3{0.0F, 0.0F, -1.0F});
            cy::f32 t = 0.0F;
            cy::Vec3 normal{0.0F, 1.0F, 0.0F};
            if (!cast(quads, eye, direction, t, normal)) {
                continue;
            }
            const size_t index = (static_cast<size_t>(y) * width) + x;
            view.position[index] = eye + (direction * t);
            view.normal[index] = normal;
            view.depth[index] = cy::dot(view.position[index] - eye, forward);
        }
    }
    return view;
}

/// The room of support.h as quads: floor, ceiling, four walls, and — 10 cm thick, as the screen
/// suites' field holds it — its divider either side of the doorway, with the cards' albedos.
[[nodiscard]] inline std::vector<AxisQuad> room_quads(cy::f32 x, cy::f32 y, cy::f32 z,
                                                      bool with_divider = true) {
    std::vector<AxisQuad> quads;
    quads.push_back({1, -y, {-x, -z}, {x, z}, {0.6F, 0.6F, 0.6F}, 1.0F});
    quads.push_back({1, y, {-x, -z}, {x, z}, {0.7F, 0.7F, 0.7F}, -1.0F});
    quads.push_back({0, -x, {-y, -z}, {y, z}, {0.85F, 0.08F, 0.06F}, 1.0F});
    quads.push_back({0, x, {-y, -z}, {y, z}, {0.08F, 0.12F, 0.85F}, -1.0F});
    quads.push_back({2, -z, {-x, -y}, {x, y}, {0.6F, 0.6F, 0.6F}, 1.0F});
    quads.push_back({2, z, {-x, -y}, {x, y}, {0.6F, 0.6F, 0.6F}, -1.0F});
    if (with_divider) {
        // A 10 cm divider, as the field holds it: two faces either side of z = 0, and the doorway's
        // two jambs between them.
        const cy::Vec3 divider{0.55F, 0.55F, 0.5F};
        quads.push_back({2, -0.05F, {-x, -y}, {-1.0F, y}, divider, -1.0F});
        quads.push_back({2, 0.05F, {-x, -y}, {-1.0F, y}, divider, 1.0F});
        quads.push_back({2, -0.05F, {1.0F, -y}, {x, y}, divider, -1.0F});
        quads.push_back({2, 0.05F, {1.0F, -y}, {x, y}, divider, 1.0F});
        quads.push_back({0, -1.0F, {-y, -0.05F}, {y, 0.05F}, divider, 1.0F});
        quads.push_back({0, 1.0F, {-y, -0.05F}, {y, 0.05F}, divider, -1.0F});
    }
    return quads;
}

/// Cards over every quad at `spacing`, at the centres of a grid of cells, facing the way the quad
/// says — both ways for a two-sided one.
[[nodiscard]] inline std::vector<cy::rendering::gi::Surfel> surfels_from_quads(
    const std::vector<AxisQuad>& quads, cy::f32 spacing, cy::u32 first_instance = 1) {
    std::vector<cy::rendering::gi::Surfel> surfels;
    cy::u32 instance = first_instance;
    for (const AxisQuad& quad : quads) {
        const cy::u32 a = quad.axis == 0 ? 1U : 0U;
        const cy::u32 b = quad.axis == 2 ? 1U : 2U;
        const auto count = [spacing](cy::f32 lo, cy::f32 hi) {
            return std::max(1U, static_cast<cy::u32>(std::lround((hi - lo) / spacing)));
        };
        const cy::u32 nu = count(quad.lo[0], quad.hi[0]);
        const cy::u32 nv = count(quad.lo[1], quad.hi[1]);
        const cy::f32 du = (quad.hi[0] - quad.lo[0]) / static_cast<cy::f32>(nu);
        const cy::f32 dv = (quad.hi[1] - quad.lo[1]) / static_cast<cy::f32>(nv);
        for (cy::u32 iu = 0; iu < nu; ++iu) {
            for (cy::u32 iv = 0; iv < nv; ++iv) {
                cy::f32 at[3] = {0.0F, 0.0F, 0.0F};
                at[quad.axis] = quad.coordinate;
                at[a] = quad.lo[0] + ((static_cast<cy::f32>(iu) + 0.5F) * du);
                at[b] = quad.lo[1] + ((static_cast<cy::f32>(iv) + 0.5F) * dv);
                for (const cy::f32 side : {1.0F, -1.0F}) {
                    if (quad.facing != 0.0F && quad.facing != side) {
                        continue;
                    }
                    cy::rendering::gi::Surfel surfel;
                    surfel.position = cy::Vec3{at[0], at[1], at[2]};
                    cy::f32 n[3] = {0.0F, 0.0F, 0.0F};
                    n[quad.axis] = side;
                    surfel.normal = cy::Vec3{n[0], n[1], n[2]};
                    surfel.albedo = quad.albedo;
                    surfel.roughness = 0.8F;
                    surfel.area = du * dv;
                    surfel.instance_id = instance;
                    surfel.material_id = instance;
                    surfels.push_back(surfel);
                }
            }
        }
        instance += 1;
    }
    return surfels;
}

/// The quads as an exact `SceneTracer` and `Occluder`: what the path tracer's ground truth
/// intersects, so the reference measures the real-time answer against the geometry rather than
/// against the field the real-time tiers trace.
class QuadTracer final : public cy::rendering::gi::SceneTracer, public cy::rendering::gi::Occluder {
public:
    explicit QuadTracer(const std::vector<AxisQuad>& quads) : quads_(&quads) {}

    [[nodiscard]] bool trace(cy::Vec3 origin, cy::Vec3 direction, cy::f32 max_distance,
                             cy::rendering::gi::SceneHit& hit) const noexcept override {
        hit = cy::rendering::gi::SceneHit{};
        const cy::Vec3 unit = cy::normalized_or(direction, cy::Vec3{0.0F, 0.0F, -1.0F});
        cy::f32 t = 0.0F;
        cy::Vec3 normal{0.0F, 1.0F, 0.0F};
        if (!cast(*quads_, origin, unit, t, normal) || t > max_distance) {
            return false;
        }
        hit.hit = true;
        hit.t = t;
        hit.position = origin + (unit * t);
        hit.normal = normal;
        hit.confidence = 1.0F;
        return true;
    }

    [[nodiscard]] bool occluded(cy::Vec3 from, cy::Vec3 to) const noexcept override {
        const cy::Vec3 offset = to - from;
        const cy::f32 span = cy::length(offset);
        if (span <= 1.0e-4F) {
            return false;
        }
        cy::f32 t = 0.0F;
        cy::Vec3 normal{0.0F, 1.0F, 0.0F};
        return cast(*quads_, from, offset / span, t, normal) && t < span * 0.999F;
    }

private:
    const std::vector<AxisQuad>* quads_;
};

}  // namespace gi_support
