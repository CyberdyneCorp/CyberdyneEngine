#pragma once
// Directional clipmaps, and the snapping without which the cache is theatre. Task 8.1.
//
// `virtual-shadows` — "Directional clipmaps and snapping". Camera-centred concentric levels of
// roughly constant shadow texel density, each covering twice the world extent of the one inside it.
//
// ================================================================================================
// SNAPPING IS THE REQUIREMENT, NOT AN OPTIMISATION, AND THE SPECIFICATION SAYS SO
// ================================================================================================
//
// "Clipmap origins SHALL be snapped to page boundaries in world space. Sub-page camera movement
// SHALL NOT move a clip level… Without snapping, small camera movement invalidates the entire cache
// every frame, producing a system that appears to cache and never does."
//
// So `clipmap_level()` floors the camera's position in the light's own basis to a whole number of
// PAGES — not texels — and the two scenarios that follow from that are the two cases in
// `test_clipmap.cpp`: a camera moving a few centimetres produces a bit-identical origin, and a
// camera crossing a boundary produces a BAND of new pages whose width is one page, not a level.
//
// Snapping to a texel would be the more obvious choice and it is wrong here: the cache's unit is a
// page, so an origin that moved by one texel would shift every page's world footprint and dirty all
// of them while looking, in a debug view, as though it had barely moved.
//
// ================================================================================================
// THE LIGHT BASIS IS BUILT ONCE AND IS NOT ARBITRARY
// ================================================================================================
//
// Two clipmap levels must agree about which way "x" points or a page in one is not the same world
// square as the page above it. `shadow_basis()` — in `address_space.h`, because every projection
// needs it — derives the right and up vectors from the light direction alone, deterministically, so
// the same light produces the same basis on every frame and on every machine. That is what makes an
// origin computed this frame comparable with the one cached last frame.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/rendering/shadows/address_space.h>

namespace cy::rendering {

struct ClipmapConfig {
    /// Concentric levels. Level 0 is the finest and covers `first_level_extent`.
    u32 level_count = 6;
    /// The world edge length covered by level 0.
    f32 first_level_extent = 32.0F;
    /// Each level covers this multiple of the one inside it. Two is the only value that keeps texel
    /// density constant per level while doubling extent; it is configurable because a project with
    /// a very long view distance may prefer fewer, coarser levels.
    f32 level_ratio = 2.0F;
    ShadowPageGeometry geometry;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return level_count != 0 && level_count <= 16 && first_level_extent > 0.0F &&
               level_ratio > 1.0F && geometry.valid();
    }
};

struct ClipmapLevel {
    u32 index = 0;
    /// World edge length the level covers.
    f32 extent = 0.0F;
    /// World edge length of one texel at this level.
    f32 texel_world_size = 0.0F;
    /// World edge length of one page at this level.
    f32 page_world_size = 0.0F;
    /// The snapped origin, in the light basis: the light-space coordinate of the level's minimum
    /// corner. Integral in units of `page_world_size`, which is what makes it comparable.
    Vec2 origin_light_space{0.0F, 0.0F};
    /// The same origin expressed in whole pages. The number the invalidation test compares.
    i64 origin_page_x = 0;
    i64 origin_page_y = 0;

    [[nodiscard]] constexpr bool same_footprint(const ClipmapLevel& other) const noexcept {
        return index == other.index && origin_page_x == other.origin_page_x &&
               origin_page_y == other.origin_page_y;
    }
};

/// Resolve one level for a camera position. Pure: the same inputs give the same level, which is the
/// property "a camera that moved two centimetres invalidates nothing" is asserted against.
[[nodiscard]] ClipmapLevel clipmap_level(const ClipmapConfig& config, const ShadowBasis& basis,
                                         Vec3 camera_position, u32 level) noexcept;

/// The finest level whose texel is no finer than `receiver_texel_world_size`, clamped to the level
/// count. A receiver two hundred metres away marks a coarse level; one at arm's length marks 0.
[[nodiscard]] u32 clipmap_level_for(const ClipmapConfig& config,
                                    f32 receiver_texel_world_size) noexcept;

/// How many pages become newly needed when a level moves from `previous` to `current` — the width
/// of the band, not the size of the level.
///
/// The specification's incremental scenario is this function returning a small number: a level that
/// shifted by one page along x on a 32×32 page level exposes 32 pages, not 1024. It returns the
/// level's whole page count when the shift is large enough that the old and new footprints do not
/// overlap, which is the honest answer for a teleport.
[[nodiscard]] u32 clipmap_pages_entering(const ClipmapConfig& config, const ClipmapLevel& previous,
                                         const ClipmapLevel& current) noexcept;

/// The one-level address space for a resolved clip level. This is the join between the snapping
/// above and the addressing in `address_space.h`: everything downstream — marking, invalidation,
/// the fallback chain — sees a `ShadowAddressSpace` and never knows a clipmap was involved.
[[nodiscard]] ShadowAddressSpace clipmap_address_space(const ClipmapConfig& config,
                                                       const ShadowBasis& basis,
                                                       const ClipmapLevel& level,
                                                       u32 light_slot) noexcept;

}  // namespace cy::rendering
