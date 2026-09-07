#include <cy/rendering/shadows/address_space.h>

#include <cy/core/math/scalar.h>

#include <cmath>
#include <cstdio>

namespace cy::rendering {
namespace {

/// The six cube faces, in the conventional +X -X +Y -Y +Z -Z order. Internal to this file: nothing
/// outside it may name a face, which is the requirement the header's second section states.
[[nodiscard]] u8 dominant_face(Vec3 v) noexcept {
    const f32 ax = std::fabs(v.x);
    const f32 ay = std::fabs(v.y);
    const f32 az = std::fabs(v.z);
    if (ax >= ay && ax >= az) {
        return v.x >= 0.0F ? 0U : 1U;
    }
    if (ay >= az) {
        return v.y >= 0.0F ? 2U : 3U;
    }
    return v.z >= 0.0F ? 4U : 5U;
}

/// The cube face's own u, v and major axis magnitude.
[[nodiscard]] Vec3 cube_face_coordinates(Vec3 v, u8 face) noexcept {
    switch (face) {
        case 0:
            return Vec3{-v.z, -v.y, std::fabs(v.x)};
        case 1:
            return Vec3{v.z, -v.y, std::fabs(v.x)};
        case 2:
            return Vec3{v.x, v.z, std::fabs(v.y)};
        case 3:
            return Vec3{v.x, -v.z, std::fabs(v.y)};
        case 4:
            return Vec3{v.x, -v.y, std::fabs(v.z)};
        default:
            return Vec3{-v.x, -v.y, std::fabs(v.z)};
    }
}

/// The octahedral map: the unit sphere folded onto [-1,1]². One face, no seams to cull against.
[[nodiscard]] Vec2 octahedral_coordinates(Vec3 v) noexcept {
    const f32 norm = std::fabs(v.x) + std::fabs(v.y) + std::fabs(v.z);
    const f32 inverse = norm > math::kSmallLength ? 1.0F / norm : 0.0F;
    Vec2 result{v.x * inverse, v.z * inverse};
    if (v.y < 0.0F) {
        const f32 x = (1.0F - std::fabs(result.y)) * (result.x >= 0.0F ? 1.0F : -1.0F);
        const f32 y = (1.0F - std::fabs(result.x)) * (result.y >= 0.0F ? 1.0F : -1.0F);
        result = Vec2{x, y};
    }
    return result;
}

/// Turn a [0,1)² coordinate into a page and a position inside it.
[[nodiscard]] ShadowAddress place(const ShadowAddressSpace& space, Vec2 unit, u8 face) noexcept {
    ShadowAddress address;
    const u32 side = space.geometry.pages_per_side();
    if (side == 0 || unit.x < 0.0F || unit.x >= 1.0F || unit.y < 0.0F || unit.y >= 1.0F) {
        return address;
    }
    const f32 fx = unit.x * static_cast<f32>(side);
    const f32 fy = unit.y * static_cast<f32>(side);
    const u32 px = math::min(static_cast<u32>(fx), side - 1U);
    const u32 py = math::min(static_cast<u32>(fy), side - 1U);
    address.page.light_slot = space.light_slot;
    address.page.level = space.level;
    address.page.face = face;
    address.page.x = static_cast<u16>(px);
    address.page.y = static_cast<u16>(py);
    address.page_uv = Vec2{fx - static_cast<f32>(px), fy - static_cast<f32>(py)};
    address.inside = true;
    return address;
}

/// The unit-square coordinate of a world position, and whether it is in front of the light at all.
/// Returns the face alongside, which is zero for everything but a point light.
struct Projected {
    Vec2 unit{0.0F, 0.0F};
    u8 face = 0;
    bool in_front = false;
};

[[nodiscard]] Projected project(const ShadowAddressSpace& space, Vec3 world_position) noexcept {
    Projected projected;
    if (space.projection == ShadowProjection::DirectionalClipmap) {
        const f32 lx = dot(world_position, space.basis.right);
        const f32 ly = dot(world_position, space.basis.up);
        const f32 extent = math::max(space.extent, math::kSmallLength);
        projected.unit = Vec2{(lx - space.origin_light_space.x) / extent,
                              (ly - space.origin_light_space.y) / extent};
        projected.in_front = true;
        return projected;
    }

    const Vec3 offset = world_position - space.position;
    if (space.projection == ShadowProjection::Spot) {
        const f32 z = dot(offset, space.basis.forward);
        if (z <= math::kSmallLength || z > space.extent) {
            return projected;
        }
        const f32 tangent = std::tan(math::clamp(space.half_angle, 1e-3F, 1.5F));
        const f32 scale = 1.0F / (z * tangent);
        projected.unit = Vec2{(dot(offset, space.basis.right) * scale * 0.5F) + 0.5F,
                              (dot(offset, space.basis.up) * scale * 0.5F) + 0.5F};
        projected.in_front = true;
        return projected;
    }

    const f32 distance = length(offset);
    if (distance <= math::kSmallLength || distance > space.extent) {
        return projected;
    }
    if (space.point_mapping == PointMapping::CubeFaces) {
        projected.face = dominant_face(offset);
        const Vec3 face_coordinates = cube_face_coordinates(offset, projected.face);
        const f32 scale = 1.0F / math::max(face_coordinates.z, math::kSmallLength);
        projected.unit = Vec2{(face_coordinates.x * scale * 0.5F) + 0.5F,
                              (face_coordinates.y * scale * 0.5F) + 0.5F};
    } else {
        const Vec2 octahedral = octahedral_coordinates(offset * (1.0F / distance));
        projected.unit = Vec2{(octahedral.x * 0.5F) + 0.5F, (octahedral.y * 0.5F) + 0.5F};
    }
    projected.in_front = true;
    return projected;
}

}  // namespace

const char* shadow_mode_name(ShadowMode mode) noexcept {
    switch (mode) {
        case ShadowMode::None:
            return "None";
        case ShadowMode::Baked:
            return "Baked";
        case ShadowMode::Conventional:
            return "Conventional";
        case ShadowMode::Virtual:
            return "Virtual";
        case ShadowMode::RayTraced:
            return "RayTraced";
        case ShadowMode::Hybrid:
            return "Hybrid";
        case ShadowMode::Count:
            break;
    }
    return "Unknown";
}

const char* shadow_projection_name(ShadowProjection projection) noexcept {
    switch (projection) {
        case ShadowProjection::DirectionalClipmap:
            return "DirectionalClipmap";
        case ShadowProjection::Spot:
            return "Spot";
        case ShadowProjection::Point:
            return "Point";
        case ShadowProjection::Count:
            break;
    }
    return "Unknown";
}

ShadowBasis shadow_basis(Vec3 light_direction) noexcept {
    ShadowBasis basis;
    const f32 magnitude = length(light_direction);
    basis.forward =
        magnitude > math::kSmallLength ? light_direction * (1.0F / magnitude) : Vec3{0, 0, -1.0F};
    // The reference axis is chosen by a comparison rather than a dot-product threshold so that the
    // same direction always picks the same one. A basis that flipped as a light rotated past a
    // threshold would relocate every page of that light in one frame.
    const Vec3 reference =
        std::fabs(basis.forward.y) < 0.9F ? Vec3{0.0F, 1.0F, 0.0F} : Vec3{1.0F, 0.0F, 0.0F};
    basis.right = normalize(cross(reference, basis.forward));
    basis.up = cross(basis.forward, basis.right);
    return basis;
}

ShadowAddress address_of(const ShadowAddressSpace& space, Vec3 world_position) noexcept {
    const Projected projected = project(space, world_position);
    if (!projected.in_front) {
        return ShadowAddress{};
    }
    return place(space, projected.unit, projected.face);
}

f32 shadow_texel_world_size(const ShadowAddressSpace& space, f32 distance) noexcept {
    const f32 texels = static_cast<f32>(space.geometry.virtual_texels);
    if (texels <= 0.0F) {
        return 0.0F;
    }
    if (space.projection == ShadowProjection::DirectionalClipmap) {
        return space.extent / texels;
    }
    // A local light's texel subtends a fixed angle, so its world footprint grows with distance.
    // The spot's is 2·tan(half angle) across the page grid; the point's is one 90° face.
    const f32 tangent = space.projection == ShadowProjection::Spot
                            ? std::tan(math::clamp(space.half_angle, 1e-3F, 1.5F))
                            : 1.0F;
    return math::max(distance, 0.0F) * 2.0F * tangent / texels;
}

u8 select_level(const ShadowAddressSpace& base_level, f32 distance, f32 receiver_texel_world_size,
                u8 level_count) noexcept {
    const u8 last = level_count == 0 ? 0U : static_cast<u8>(level_count - 1U);
    const f32 base = shadow_texel_world_size(base_level, distance);
    if (base <= 0.0F || receiver_texel_world_size <= base) {
        return 0;
    }
    u8 level = 0;
    f32 texel = base;
    while (level < last && texel < receiver_texel_world_size) {
        texel *= 2.0F;
        ++level;
    }
    return level;
}

u32 pages_covering(const ShadowAddressSpace& space, const Aabb& bounds, VirtualPage* out,
                   u32 out_capacity) noexcept {
    const u32 side = space.geometry.pages_per_side();
    if (side == 0 || bounds.is_empty()) {
        return 0;
    }

    // The eight corners rather than the box's own projection: a projective mapping does not
    // preserve an axis-aligned rectangle, and the corner hull is the cheapest correct answer.
    f32 min_x = math::kInfinity;
    f32 min_y = math::kInfinity;
    f32 max_x = -math::kInfinity;
    f32 max_y = -math::kInfinity;
    u32 faces = 0;
    u8 first_face = 0;
    u32 in_front = 0;
    for (u32 corner = 0; corner < 8; ++corner) {
        const Vec3 point{(corner & 1U) != 0U ? bounds.max.x : bounds.min.x,
                         (corner & 2U) != 0U ? bounds.max.y : bounds.min.y,
                         (corner & 4U) != 0U ? bounds.max.z : bounds.min.z};
        const Projected projected = project(space, point);
        if (!projected.in_front) {
            continue;
        }
        if (in_front == 0) {
            first_face = projected.face;
        }
        faces |= 1U << projected.face;
        ++in_front;
        min_x = math::min(min_x, projected.unit.x);
        min_y = math::min(min_y, projected.unit.y);
        max_x = math::max(max_x, projected.unit.x);
        max_y = math::max(max_y, projected.unit.y);
    }
    if (in_front == 0) {
        return 0;
    }
    // A box straddling more than one cube face is dirtied conservatively on the face its first
    // corner landed on plus the clamped rectangle; the alternative is a per-face clip, which costs
    // more than re-rendering the handful of extra pages it would save.
    (void)faces;

    const f32 side_f = static_cast<f32>(side);
    const i32 last = static_cast<i32>(side) - 1;
    const i32 x0 = math::clamp(static_cast<i32>(std::floor(min_x * side_f)), 0, last);
    const i32 y0 = math::clamp(static_cast<i32>(std::floor(min_y * side_f)), 0, last);
    const i32 x1 = math::clamp(static_cast<i32>(std::floor(max_x * side_f)), 0, last);
    const i32 y1 = math::clamp(static_cast<i32>(std::floor(max_y * side_f)), 0, last);

    u32 wanted = 0;
    for (i32 y = y0; y <= y1; ++y) {
        for (i32 x = x0; x <= x1; ++x) {
            if (wanted < out_capacity && out != nullptr) {
                VirtualPage page;
                page.light_slot = space.light_slot;
                page.level = space.level;
                page.face = first_face;
                page.x = static_cast<u16>(x);
                page.y = static_cast<u16>(y);
                out[wanted] = page;
            }
            ++wanted;
        }
    }
    return wanted;
}

usize describe_page_geometry(const ShadowPageGeometry& geometry, char* out,
                             usize capacity) noexcept {
    if (out == nullptr || capacity == 0) {
        return 0;
    }
    const int written = std::snprintf(out, capacity, "%u texels/page, %u^2 virtual, %ux%u pages",
                                      geometry.page_texels, geometry.virtual_texels,
                                      geometry.pages_per_side(), geometry.pages_per_side());
    return written <= 0 ? 0 : math::min(static_cast<usize>(written), capacity - 1);
}

}  // namespace cy::rendering
