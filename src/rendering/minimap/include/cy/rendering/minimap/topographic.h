// SPDX-License-Identifier: MIT
#pragma once
// A topographic minimap: the ground drawn from above as a map is drawn — its painted colours,
// hillshading from the height field, contour lines at a chosen interval, water and roads — with a
// team's fog of war over it and the units and buildings it may see as icons.
// openspec/changes/add-rts-combat-and-minimap.
//
// THREE STAGES, EACH CHEAPER AND MORE FREQUENT THAN THE LAST:
//
//   bake       once, when the map loads: `bake_topographic` turns the heights and the ground's
//              colours into an opaque RGBA8 image — shaded by a light from the image's top left,
//              cut by contour lines (`classify_contours`), water tinted by its depth, roads laid
//              over. Nothing in it changes while the map stands.
//   fog        when the team's field changes: `apply_minimap_fog` draws the bake through the same
//              field the frame samples (fog_of_war's `sample_visibility`): a visible point is the
//              bake, an explored one is the bake greyed and darkened, a never-explored one black.
//   icons      every time the image is shown: `draw_icons` stamps the units and buildings the
//              caller decided the team may see — a dot a unit, a square a building — on a copy.
//
// `TopographicMinimap` holds the three together and does the fog stage only where the field's
// texels differ from the last ones it drew — the texels over a changed cell or its neighbours —
// so a minimap shown every frame redraws the ground its units' sight moved over, and on a frame
// nothing moved, only its icons.
//
// THE PICTURE IS RGBA8 WITH RED IN THE LOW BYTE, opaque — what `ui::render::UiRenderer::
// upload_atlas` takes for an image page — and row 0 is the region's `top_z` edge, as
// fog_of_war's minimap overlay is, so the two can never disagree about where anything is.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/fog_of_war/minimap.h>
#include <cy/rendering/fog_of_war/visibility_field.h>

namespace cy::rendering::minimap {

using fog_of_war::MinimapRegion;

/// A height field over the ground: `columns * rows` samples row-major, sample (0, 0) at
/// (`origin_x`, `origin_z`), `spacing` metres apart along both axes.
struct HeightGrid {
    Span<const f32> heights;
    u32 columns = 0;
    u32 rows = 0;
    f32 origin_x = 0.0F;
    f32 origin_z = 0.0F;
    f32 spacing = 1.0F;

    /// The height at a world point, bilinear between samples, the edge clamped.
    [[nodiscard]] f32 at(f32 x, f32 z) const noexcept;
};

/// What a texel of the contour layer is.
enum class Contour : u8 { None = 0, Line = 1, Index = 2, Shore = 3 };

struct TopographicStyle {
    /// Metres between two contour lines, > 0; every `index_every`th one is an index contour.
    f32 contour_interval = 2.0F;
    u32 index_every = 5;
    /// Ground below this is water. The default has none.
    f32 water_level = -1.0e30F;
    /// Metres of depth at which the water reaches its deep colour.
    f32 water_depth = 4.0F;
    /// Where the light comes from, as image directions — x right, y down the image — and its
    /// elevation in radians: the cartographic convention is the top left, 45 degrees up.
    f32 light_x = -1.0F;
    f32 light_y = -1.0F;
    f32 light_elevation = 0.7853982F;
    /// How much the hillshade darkens and brightens: 0 flat colour, 1 the full Lambert term.
    f32 shade_strength = 0.65F;
    /// Colours are 0xRRGGBB; alphas 0 to 1 over the ground.
    u32 ground_colour = 0x8A8A7AU;
    u32 contour_colour = 0x3B2A16U;
    f32 contour_alpha = 0.32F;
    f32 index_alpha = 0.62F;
    u32 shallow_colour = 0x5C93BFU;
    u32 deep_colour = 0x1F4F86U;
    u32 shore_colour = 0x173C66U;
    u32 road_colour = 0xC9B58AU;
    f32 road_alpha = 0.85F;
};

/// What `bake_topographic` reads. `colours` and `roads` are optional: empty, the ground is the
/// style's colour and nothing is a road; otherwise `width * height` of each, the image's own
/// texels — the painted layers' colours as RGBA8 (red low, alpha ignored), and a road's coverage.
struct TopographicInput {
    HeightGrid heights;
    MinimapRegion region;
    u32 width = 0;
    u32 height = 0;
    Span<const u32> colours;
    Span<const u8> roads;
};

/// The world point under texel (`column`, `row`)'s centre.
[[nodiscard]] Vec2 texel_point(const MinimapRegion& region, u32 width, u32 height, u32 column,
                               u32 row) noexcept;

/// The contour layer of a height image: `width * height` heights, one a texel, row-major. A texel
/// is on a line when a level `k * interval` lies between it and one of its four neighbours and it
/// is the HIGHER of the two (its height at or above the level, the neighbour's below), so a line
/// is one texel wide and sits on the uphill side of where the level crosses. A level that is a
/// multiple of `index_every` lines is an index contour. Ground below `water_level` has no line;
/// a texel above it beside one below it is the shore.
[[nodiscard]] Status classify_contours(Span<const f32> heights, u32 width, u32 height,
                                       const TopographicStyle& style, Span<Contour> out) noexcept;

/// The bake: `width * height` opaque texels into `out`.
[[nodiscard]] Status bake_topographic(const TopographicInput& input, const TopographicStyle& style,
                                      Span<u32> out) noexcept;

/// How the fog is drawn over the bake.
struct MinimapFogLook {
    /// An explored, unseen point: the bake's luminance times this, as grey.
    f32 explored_brightness = 0.55F;
};

/// `base` through a team's field: visible as it is, explored greyed, never explored black.
/// `texels` is the field fog_of_war's `VisibilityField` packs, `grid_width * grid_height` of them.
[[nodiscard]] Status apply_minimap_fog(Span<const u32> base, u32 width, u32 height,
                                       const MinimapRegion& region, Span<const u8> texels,
                                       u32 grid_width, u32 grid_height,
                                       const fog_of_war::FogGridPlacement& placement,
                                       const MinimapFogLook& look, Span<u32> out) noexcept;

/// The shape an icon is stamped as.
enum class IconShape : u8 { Dot = 0, Square = 1 };

/// A unit or a building on the minimap, at a world point.
struct MinimapIcon {
    f32 x = 0.0F;
    f32 z = 0.0F;
    /// 0xRRGGBB.
    u32 colour = 0xFFFFFFU;
    IconShape shape = IconShape::Dot;
    /// Its radius — or half side — in texels, the outline included.
    f32 radius = 2.0F;
    /// A dark one-texel outline, so an icon reads on any ground.
    bool outline = true;
};

/// Stamp `icons` on `image` in order, later over earlier. Off the region is not drawn.
void draw_icons(Span<const MinimapIcon> icons, const MinimapRegion& region, u32 width, u32 height,
                Span<u32> image) noexcept;

/// The three stages kept together, the fog stage only when the field changed.
class TopographicMinimap {
public:
    explicit TopographicMinimap(Allocator& allocator) noexcept;

    /// Bake the map. Required before `compose`.
    [[nodiscard]] Status bake(const TopographicInput& input,
                              const TopographicStyle& style) noexcept;

    /// The picture to show: the bake through the field — redrawn whole the first time or when the
    /// grid's shape changes, and after that only over the cells that changed since the last
    /// composition — and `icons` over it.
    [[nodiscard]] Status compose(Span<const u8> texels, u32 grid_width, u32 grid_height,
                                 const fog_of_war::FogGridPlacement& placement,
                                 Span<const MinimapIcon> icons) noexcept;

    [[nodiscard]] u32 width() const noexcept { return width_; }
    [[nodiscard]] u32 height() const noexcept { return height_; }
    [[nodiscard]] const MinimapRegion& region() const noexcept { return region_; }
    [[nodiscard]] Span<const u32> base() const noexcept { return base_.span(); }
    [[nodiscard]] Span<const u32> fogged() const noexcept { return fogged_.span(); }
    [[nodiscard]] Span<const u32> image() const noexcept { return image_.span(); }
    /// How many compositions ran the fog stage, how many reused it whole, and how many texels the
    /// fog stage has drawn over them all.
    [[nodiscard]] u64 fog_passes() const noexcept { return fog_passes_; }
    [[nodiscard]] u64 reused() const noexcept { return reused_; }
    [[nodiscard]] u64 refogged() const noexcept { return refogged_; }

    MinimapFogLook look;

private:
    [[nodiscard]] Status refog_changed(Span<const u8> texels,
                                       const fog_of_war::FogGridPlacement& placement) noexcept;

    Array<u32> base_;
    Array<u32> fogged_;
    Array<u32> image_;
    /// The field the fogged image was drawn through, and the cells to redraw this time.
    Array<u8> field_;
    Array<u8> dirty_;
    MinimapRegion region_;
    u32 width_ = 0;
    u32 height_ = 0;
    u32 grid_width_ = 0;
    u32 grid_height_ = 0;
    bool fogged_once_ = false;
    u64 fog_passes_ = 0;
    u64 reused_ = 0;
    u64 refogged_ = 0;
};

}  // namespace cy::rendering::minimap
