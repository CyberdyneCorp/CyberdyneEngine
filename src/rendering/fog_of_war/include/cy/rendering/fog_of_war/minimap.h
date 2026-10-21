// SPDX-License-Identifier: MIT
#pragma once
// Fog of war on the minimap: the same field the frame samples, drawn as an overlay image the HUD's
// minimap shows between its ground and its unit dots.
//
// The overlay is premultiplied RGBA8 with red in the low byte — what `ui::render::UiRenderer::
// upload_atlas` takes for an image page — and each texel is the field sampled by
// `sample_visibility` at the world point under the texel's centre, so the minimap and the frame
// cannot disagree about where the fog is: a visible point is clear, an explored one is the
// explored tint, and a never-explored one is the unexplored colour. A minimap cannot desaturate
// what is under it, so where the frame greys an explored cell the minimap darkens it.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/fog_of_war/visibility_field.h>

namespace cy::rendering::fog_of_war {

/// The world rectangle the minimap shows. Image row 0 is `top_z` and the last row `bottom_z`, so a
/// map drawn north-up names its far edge first.
struct MinimapRegion {
    f32 left_x = 0.0F;
    f32 right_x = 1.0F;
    f32 top_z = 0.0F;
    f32 bottom_z = 1.0F;
};

struct MinimapFogStyle {
    /// Coverage over an explored point that is not visible, and over a never-explored one.
    f32 explored_alpha = 0.55F;
    f32 unexplored_alpha = 1.0F;
    /// The fog's colour, 0xRRGGBB.
    u32 colour = 0x000000U;
};

/// One texel's premultiplied colour, red in the low byte, for a sample of the field.
[[nodiscard]] u32 minimap_fog_texel(const FogSample& sample, const MinimapFogStyle& style) noexcept;

/// The overlay: `width * height` texels, row-major from the top-left, into `out`.
[[nodiscard]] Status compose_minimap_fog(Span<const u8> texels, u32 grid_width, u32 grid_height,
                                         const FogGridPlacement& placement,
                                         const MinimapRegion& region, u32 width, u32 height,
                                         const MinimapFogStyle& style, Span<u32> out) noexcept;

/// Whether a unit's dot is on a team's minimap, from the simulation's sight of it
/// (`cy::gameplay::fog::UnitSight`: 0 hidden, 1 ghost, 2 visible). A ghost keeps its dot where it
/// was last seen; a hidden unit has none.
[[nodiscard]] constexpr bool minimap_shows(u8 sight) noexcept {
    return sight != 0U;
}

}  // namespace cy::rendering::fog_of_war
