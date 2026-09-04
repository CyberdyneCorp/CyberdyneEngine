#include "scene.h"

#include <cy/core/base/assert.h>
#include <cy/core/math/projection.h>
#include <cy/core/math/scalar.h>

#include <cmath>
#include <numbers>

namespace cy::sample::first_light {
namespace {

/// The ground is a square in the XZ plane. Eight metres of half extent is enough that the boxes'
/// shadows land on it at every phase of the orbit and small enough that the shadow map's 1024
/// texels are about eight millimetres each.
constexpr f32 kGroundHalfExtent = 8.0F;
/// How many times the checkerboard repeats across the ground. The boxes use 1, so the same texture
/// reads as a floor pattern below and as a material above.
constexpr f32 kGroundUvScale = 4.0F;
constexpr f32 kRingRadius = 4.2F;
constexpr f32 kPillarHeight = 2.6F;

/// The camera's orbit. A sample whose camera did not move could not show that the shadow, the
/// specular-free Lambert term and the camera-relative transform all track it.
constexpr f32 kOrbitRadius = 9.5F;
constexpr f32 kOrbitHeight = 4.4F;
constexpr f32 kOrbitTargetHeight = 1.3F;
constexpr f32 kFieldOfViewY = 0.9F;  // radians, about 52 degrees

/// One corner of one face, written into the arrays.
struct Corner {
    Vec3 position;
    Vec3 normal;
    f32 u = 0.0F;
    f32 v = 0.0F;
};

void write_vertex(Vertex& out, const Corner& corner) noexcept {
    out.position[0] = corner.position.x;
    out.position[1] = corner.position.y;
    out.position[2] = corner.position.z;
    out.normal[0] = corner.normal.x;
    out.normal[1] = corner.normal.y;
    out.normal[2] = corner.normal.z;
    out.uv[0] = corner.u;
    out.uv[1] = corner.v;
}

/// The camera-relative position of a world point, computed the only way that survives a million
/// units: subtract in `f64`, narrow afterwards.
Vec3 relative_to(const f64 world[3], const f64 camera[3]) noexcept {
    return Vec3{static_cast<f32>(world[0] - camera[0]), static_cast<f32>(world[1] - camera[1]),
                static_cast<f32>(world[2] - camera[2])};
}

}  // namespace

Scene::Scene(Allocator& allocator) noexcept
    : vertices_(allocator), indices_(allocator), objects_(allocator), checker_(allocator) {}

Status Scene::build(const SceneDescription& description) noexcept {
    description_ = description;
    centre_[0] = description.origin;
    centre_[1] = description.origin;
    centre_[2] = description.origin;

    sun_.direction = normalize(Vec3{0.45F, 0.82F, 0.36F});
    sun_.color = Vec3{1.0F, 0.96F, 0.88F} * 1.05F;
    sun_.ambient = 0.10F;

    if (Status built = build_geometry(); !built) {
        return built;
    }
    return build_checker();
}

Status Scene::build_geometry() noexcept {
    vertices_.clear();
    indices_.clear();
    objects_.clear();

    // The ground quad, wound counter-clockwise seen from +Y so that its normal and its winding
    // agree — the pipeline does not cull, but a mesh whose winding contradicted its normal would be
    // a trap for whoever turns culling on.
    const u32 ground_first = static_cast<u32>(indices_.size());
    const Corner ground[4] = {
        {{-kGroundHalfExtent, 0.0F, -kGroundHalfExtent}, {0.0F, 1.0F, 0.0F}, 0.0F, 0.0F},
        {{-kGroundHalfExtent, 0.0F, kGroundHalfExtent}, {0.0F, 1.0F, 0.0F}, 0.0F, kGroundUvScale},
        {{kGroundHalfExtent, 0.0F, kGroundHalfExtent},
         {0.0F, 1.0F, 0.0F},
         kGroundUvScale,
         kGroundUvScale},
        {{kGroundHalfExtent, 0.0F, -kGroundHalfExtent}, {0.0F, 1.0F, 0.0F}, kGroundUvScale, 0.0F},
    };
    const auto ground_base = static_cast<u16>(vertices_.size());
    for (const Corner& corner : ground) {
        Vertex vertex;
        write_vertex(vertex, corner);
        if (Status pushed = vertices_.push_back(vertex); !pushed) {
            return pushed;
        }
    }
    const u16 ground_indices[6] = {0, 1, 2, 0, 2, 3};
    for (const u16 index : ground_indices) {
        if (Status pushed = indices_.push_back(static_cast<u16>(ground_base + index)); !pushed) {
            return pushed;
        }
    }
    const auto ground_count = static_cast<u32>(indices_.size()) - ground_first;

    // One unit box, shared by the pillar and by every box in the ring. Instancing is the render
    // server's subject; what this file demonstrates is that one mesh range serves many objects
    // because the transform is per-draw and the geometry is not.
    u32 box_first = 0;
    u32 box_count = 0;
    if (Status appended = append_box(Vec3{0.5F, 0.5F, 0.5F}, 1.0F, box_first, box_count);
        !appended) {
        return appended;
    }

    Object floor;
    floor.world_position[0] = centre_[0];
    floor.world_position[1] = centre_[1];
    floor.world_position[2] = centre_[2];
    floor.base_color[0] = 0.72F;
    floor.base_color[1] = 0.74F;
    floor.base_color[2] = 0.78F;
    floor.first_index = ground_first;
    floor.index_count = ground_count;
    if (Status pushed = objects_.push_back(floor); !pushed) {
        return pushed;
    }

    Object pillar;
    pillar.world_position[0] = centre_[0];
    pillar.world_position[1] = centre_[1] + (static_cast<f64>(kPillarHeight) * 0.5);
    pillar.world_position[2] = centre_[2];
    pillar.yaw_radians = 0.35F;
    pillar.scale = kPillarHeight;
    pillar.base_color[0] = 0.90F;
    pillar.base_color[1] = 0.55F;
    pillar.base_color[2] = 0.30F;
    pillar.first_index = box_first;
    pillar.index_count = box_count;
    if (Status pushed = objects_.push_back(pillar); !pushed) {
        return pushed;
    }

    // The ring. Sizes and colours vary with the index rather than with a random draw, so the scene
    // is a pure function of `--boxes` and the golden references do not depend on a seed.
    for (u32 index = 0; index < description_.box_count; ++index) {
        const f32 turn =
            static_cast<f32>(index) / static_cast<f32>(math::max(description_.box_count, 1U));
        const f32 angle = turn * 2.0F * math::kPi;
        const f32 scale = 0.55F + ((0.45F * static_cast<f32>(index % 3U)) / 2.0F);

        Object box;
        box.world_position[0] = centre_[0] + static_cast<f64>(std::cos(angle) * kRingRadius);
        box.world_position[1] = centre_[1] + (static_cast<f64>(scale) * 0.5);
        box.world_position[2] = centre_[2] + static_cast<f64>(std::sin(angle) * kRingRadius);
        box.yaw_radians = angle;
        box.scale = scale;
        box.base_color[0] = 0.30F + (0.60F * turn);
        box.base_color[1] = 0.85F - (0.45F * turn);
        box.base_color[2] = 0.40F + (0.50F * (1.0F - turn));
        box.first_index = box_first;
        box.index_count = box_count;
        if (Status pushed = objects_.push_back(box); !pushed) {
            return pushed;
        }
    }

    // A sphere around everything, which is what the shadow projection is fitted to. The ground's
    // diagonal dominates it, so it does not change with `--boxes` — which is the point: a shadow
    // map whose texel size depended on the object count would make two runs incomparable.
    // The ground is a square, so its bounding circle's radius is the half extent times root two.
    bounding_radius_ = kGroundHalfExtent * std::numbers::sqrt2_v<f32>;
    return ok();
}

Status Scene::append_box(Vec3 half_extents, f32 uv_scale, u32& first_index,
                         u32& index_count) noexcept {
    first_index = static_cast<u32>(indices_.size());
    const auto base = static_cast<u16>(vertices_.size());

    // Six faces, each with its own four vertices, because a cube's corners have three different
    // normals and a shared vertex could carry only one of them.
    const Vec3 normals[6] = {{1.0F, 0.0F, 0.0F},  {-1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F},
                             {0.0F, -1.0F, 0.0F}, {0.0F, 0.0F, 1.0F},  {0.0F, 0.0F, -1.0F}};
    // For each face: the two in-plane axes, in the order that makes the winding counter-clockwise
    // when seen from outside. THE INVARIANT IS `cross(tangent, bitangent) == normal`, and it is
    // worth stating because getting one of the six wrong is invisible until you look: the +Y row
    // was `{1,0,0} x {0,0,1}`, which is −Y, so every box in this scene rendered without a top face
    // and the sample's first image looked like a set of open shells.
    const Vec3 tangents[6] = {{0.0F, 0.0F, -1.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 1.0F},
                              {-1.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {-1.0F, 0.0F, 0.0F}};
    const Vec3 bitangents[6] = {{0.0F, 1.0F, 0.0F},  {0.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 0.0F},
                                {0.0F, 0.0F, -1.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 1.0F, 0.0F}};
    for (u32 face = 0; face < 6; ++face) {
        CY_ASSERT(nearly_equal(cross(tangents[face], bitangents[face]), normals[face]));
    }

    for (u32 face = 0; face < 6; ++face) {
        const Vec3 normal = normals[face];
        const Vec3 tangent = tangents[face];
        const Vec3 bitangent = bitangents[face];
        const Vec3 centre{normal.x * half_extents.x, normal.y * half_extents.y,
                          normal.z * half_extents.z};
        const f32 corners[4][2] = {{-1.0F, -1.0F}, {1.0F, -1.0F}, {1.0F, 1.0F}, {-1.0F, 1.0F}};
        for (const auto& corner : corners) {
            const Vec3 along = Vec3{tangent.x * half_extents.x, tangent.y * half_extents.y,
                                    tangent.z * half_extents.z} *
                               corner[0];
            const Vec3 up = Vec3{bitangent.x * half_extents.x, bitangent.y * half_extents.y,
                                 bitangent.z * half_extents.z} *
                            corner[1];
            Vertex vertex;
            write_vertex(vertex,
                         Corner{centre + along + up, normal, ((corner[0] * 0.5F) + 0.5F) * uv_scale,
                                ((corner[1] * 0.5F) + 0.5F) * uv_scale});
            if (Status pushed = vertices_.push_back(vertex); !pushed) {
                return pushed;
            }
        }
        const u16 face_base = static_cast<u16>(base + (face * 4U));
        const u16 order[6] = {0, 1, 2, 0, 2, 3};
        for (const u16 index : order) {
            if (Status pushed = indices_.push_back(static_cast<u16>(face_base + index)); !pushed) {
                return pushed;
            }
        }
    }
    index_count = static_cast<u32>(indices_.size()) - first_index;
    return ok();
}

Status Scene::build_checker() noexcept {
    if (Status sized = checker_.resize(static_cast<usize>(kCheckerExtent) * kCheckerExtent);
        !sized) {
        return sized;
    }
    // Eight-texel squares, two greys. Rgba8Unorm is little-endian byte order in a `u32`: red in the
    // low byte, which is the order the readback in the golden suite reads it back in.
    for (u32 y = 0; y < kCheckerExtent; ++y) {
        for (u32 x = 0; x < kCheckerExtent; ++x) {
            const bool light = (((x / 8U) + (y / 8U)) % 2U) == 0U;
            const u32 level = light ? 0xE8U : 0x60U;
            checker_[(static_cast<usize>(y) * kCheckerExtent) + x] =
                level | (level << 8U) | (level << 16U) | 0xFF000000U;
        }
    }
    return ok();
}

Camera Scene::camera_at(f32 phase_turns) const noexcept {
    const f32 angle = phase_turns * 2.0F * math::kPi;
    Camera camera;
    camera.position[0] = centre_[0] + static_cast<f64>(std::cos(angle) * kOrbitRadius);
    camera.position[1] = centre_[1] + static_cast<f64>(kOrbitHeight);
    camera.position[2] = centre_[2] + static_cast<f64>(std::sin(angle) * kOrbitRadius);

    // The target, expressed as a camera-relative direction rather than as a world point — the
    // subtraction happens in `f64` here, once, and nothing downstream sees a world coordinate.
    const f64 target[3] = {centre_[0], centre_[1] + static_cast<f64>(kOrbitTargetHeight),
                           centre_[2]};
    camera.forward = normalize(relative_to(target, camera.position));
    camera.up = kAxisUp;
    camera.fov_y_radians = kFieldOfViewY;
    camera.near_plane = 0.1F;
    return camera;
}

Mat4 view_projection(const Camera& camera, u32 width, u32 height) noexcept {
    const f32 aspect =
        height == 0 ? 1.0F : static_cast<f32>(width) / static_cast<f32>(math::max(height, 1U));
    // The camera sits at the origin of the space its own positions are relative to, so the view
    // matrix is a rotation with no translation in it at all. That is the whole of camera-relative
    // rendering on this side of the boundary.
    const Mat4 view = look_at(Vec3{0.0F, 0.0F, 0.0F}, camera.forward, camera.up);
    const Mat4 projection =
        perspective_reversed_z_infinite(camera.fov_y_radians, aspect, camera.near_plane);
    return projection * view;
}

Mat4 sun_view_projection(const Scene& scene, const Camera& camera) noexcept {
    const Sun& sun = scene.sun();
    const f32 radius = scene.bounding_radius();
    // The scene's centre, camera-relative. The light's frustum is placed against it, so the light's
    // own position is never a world coordinate either.
    const Vec3 centre = relative_to(scene.centre(), camera.position);
    const Vec3 eye = centre + (sun.direction * (radius + 1.0F));
    // An up vector the direction is not parallel to. `any_perpendicular` picks the axis the
    // direction is least aligned with, which is exactly the choice that keeps `look_at` well
    // conditioned.
    const Vec3 up = any_perpendicular(sun.direction);
    const Mat4 view = look_at(eye, centre, up);
    const Mat4 projection =
        orthographic_reversed_z(-radius, radius, -radius, radius, 0.5F, (radius * 2.0F) + 2.0F);
    return projection * view;
}

}  // namespace cy::sample::first_light
