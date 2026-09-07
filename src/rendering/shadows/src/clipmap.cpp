#include <cy/rendering/shadows/clipmap.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering {
namespace {

/// Floor to a whole number of pages. `std::floor` and not a cast: a cast truncates toward zero, so
/// a camera at x = -0.5 pages would snap to 0 on one side of the origin and to -1 on the other, and
/// the discontinuity would sit exactly where a level crosses the world origin.
[[nodiscard]] i64 floor_pages(f32 coordinate, f32 page_world_size) noexcept {
    return static_cast<i64>(std::floor(coordinate / page_world_size));
}

}  // namespace

ClipmapLevel clipmap_level(const ClipmapConfig& config, const ShadowBasis& basis,
                           Vec3 camera_position, u32 level) noexcept {
    ClipmapLevel resolved;
    if (!config.valid()) {
        return resolved;
    }
    const u32 index = math::min(level, config.level_count - 1U);
    const u32 side = config.geometry.pages_per_side();
    if (side == 0) {
        return resolved;
    }

    f32 extent = config.first_level_extent;
    for (u32 step = 0; step < index; ++step) {
        extent *= config.level_ratio;
    }

    resolved.index = index;
    resolved.extent = extent;
    resolved.texel_world_size = extent / static_cast<f32>(config.geometry.virtual_texels);
    resolved.page_world_size = extent / static_cast<f32>(side);

    // Centre on the camera, then snap the MINIMUM CORNER down to a page boundary. Snapping the
    // centre would leave the corner on a half-page and shift every page's world footprint by half a
    // page relative to the level above, which stops two levels sharing a lattice.
    const f32 centre_x = dot(camera_position, basis.right);
    const f32 centre_y = dot(camera_position, basis.up);
    resolved.origin_page_x = floor_pages(centre_x - (extent * 0.5F), resolved.page_world_size);
    resolved.origin_page_y = floor_pages(centre_y - (extent * 0.5F), resolved.page_world_size);
    resolved.origin_light_space =
        Vec2{static_cast<f32>(resolved.origin_page_x) * resolved.page_world_size,
             static_cast<f32>(resolved.origin_page_y) * resolved.page_world_size};
    return resolved;
}

u32 clipmap_level_for(const ClipmapConfig& config, f32 receiver_texel_world_size) noexcept {
    if (!config.valid()) {
        return 0;
    }
    const f32 base = config.first_level_extent / static_cast<f32>(config.geometry.virtual_texels);
    if (base <= 0.0F || receiver_texel_world_size <= base) {
        return 0;
    }
    u32 level = 0;
    f32 texel = base;
    while (level + 1U < config.level_count && texel < receiver_texel_world_size) {
        texel *= config.level_ratio;
        ++level;
    }
    return level;
}

u32 clipmap_pages_entering(const ClipmapConfig& config, const ClipmapLevel& previous,
                           const ClipmapLevel& current) noexcept {
    const u32 side = config.geometry.pages_per_side();
    if (side == 0) {
        return 0;
    }
    const u32 total = side * side;
    if (previous.index != current.index) {
        return total;
    }
    const i64 shift_x = current.origin_page_x - previous.origin_page_x;
    const i64 shift_y = current.origin_page_y - previous.origin_page_y;
    const i64 span = static_cast<i64>(side);
    const i64 overlap_x = span - (shift_x < 0 ? -shift_x : shift_x);
    const i64 overlap_y = span - (shift_y < 0 ? -shift_y : shift_y);
    if (overlap_x <= 0 || overlap_y <= 0) {
        return total;
    }
    return total - static_cast<u32>(overlap_x * overlap_y);
}

ShadowAddressSpace clipmap_address_space(const ClipmapConfig& config, const ShadowBasis& basis,
                                         const ClipmapLevel& level, u32 light_slot) noexcept {
    ShadowAddressSpace space;
    space.projection = ShadowProjection::DirectionalClipmap;
    space.geometry = config.geometry;
    space.light_slot = light_slot;
    space.level = static_cast<u8>(math::min(level.index, 15U));
    space.basis = basis;
    space.origin_light_space = level.origin_light_space;
    space.extent = level.extent;
    return space;
}

}  // namespace cy::rendering
