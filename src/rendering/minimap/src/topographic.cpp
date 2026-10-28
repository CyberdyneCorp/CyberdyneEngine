// SPDX-License-Identifier: MIT
// The topographic minimap. See cy/rendering/minimap/topographic.h.

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/minimap/topographic.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cy::rendering::minimap {
namespace {

struct Rgb {
    f32 r = 0.0F;
    f32 g = 0.0F;
    f32 b = 0.0F;
};

[[nodiscard]] Rgb unpack_rgb(u32 colour) noexcept {
    return Rgb{static_cast<f32>((colour >> 16U) & 0xFFU), static_cast<f32>((colour >> 8U) & 0xFFU),
               static_cast<f32>(colour & 0xFFU)};
}

/// An RGBA8 texel, red in the low byte.
[[nodiscard]] Rgb unpack_texel(u32 texel) noexcept {
    return Rgb{static_cast<f32>(texel & 0xFFU), static_cast<f32>((texel >> 8U) & 0xFFU),
               static_cast<f32>((texel >> 16U) & 0xFFU)};
}

[[nodiscard]] u32 pack_texel(Rgb colour) noexcept {
    const auto channel = [](f32 value) noexcept {
        return static_cast<u32>(std::lround(std::clamp(value, 0.0F, 255.0F)));
    };
    return channel(colour.r) | (channel(colour.g) << 8U) | (channel(colour.b) << 16U) |
           0xFF00'0000U;
}

[[nodiscard]] Rgb mix(Rgb a, Rgb b, f32 t) noexcept {
    return Rgb{a.r + ((b.r - a.r) * t), a.g + ((b.g - a.g) * t), a.b + ((b.b - a.b) * t)};
}

[[nodiscard]] Rgb scale(Rgb colour, f32 k) noexcept {
    return Rgb{colour.r * k, colour.g * k, colour.b * k};
}

[[nodiscard]] bool sized(u32 width, u32 height, usize count) noexcept {
    return width > 0 && height > 0 && count == static_cast<usize>(width) * height;
}

/// The level index of the highest contour level at or below `h`.
[[nodiscard]] i64 level_below(f32 h, f32 interval) noexcept {
    return static_cast<i64>(std::floor(static_cast<f64>(h) / static_cast<f64>(interval)));
}

/// Whether the texel at `here` lies on a line against `there`, and which: the highest level `L`
/// with `there < L <= here`.
[[nodiscard]] Contour crossing(f32 here, f32 there, const TopographicStyle& style) noexcept {
    if (!(here > there)) {
        return Contour::None;
    }
    const i64 top = level_below(here, style.contour_interval);
    const f64 level = static_cast<f64>(top) * static_cast<f64>(style.contour_interval);
    if (!(static_cast<f64>(there) < level)) {
        return Contour::None;
    }
    // Any level in (there, here] makes a line; the index one wins when one lies among them.
    const i64 bottom = level_below(there, style.contour_interval) + 1;
    for (i64 k = bottom; k <= top && style.index_every > 0; ++k) {
        if (k % static_cast<i64>(style.index_every) == 0) {
            return Contour::Index;
        }
    }
    return Contour::Line;
}

[[nodiscard]] Contour stronger(Contour a, Contour b) noexcept {
    return static_cast<u8>(a) >= static_cast<u8>(b) ? a : b;
}

/// The hillshade factor of the texel at (`column`, `row`) of a height image: the Lambert term of
/// the light over the flat ground's, eased by the style's strength.
[[nodiscard]] f32 hillshade(Span<const f32> heights, u32 width, u32 height, u32 column, u32 row,
                            f32 metres_x, f32 metres_z, const TopographicStyle& style) noexcept {
    const auto h = [&](u32 c, u32 r) noexcept {
        return heights[(static_cast<usize>(r) * width) + c];
    };
    const u32 left = column > 0 ? column - 1U : column;
    const u32 right = column + 1U < width ? column + 1U : column;
    const u32 up = row > 0 ? row - 1U : row;
    const u32 down = row + 1U < height ? row + 1U : row;
    const f32 dx =
        (h(right, row) - h(left, row)) / (metres_x * static_cast<f32>(std::max(1U, right - left)));
    const f32 dy =
        (h(column, down) - h(column, up)) / (metres_z * static_cast<f32>(std::max(1U, down - up)));
    // The surface normal in image space (x right, y down, up out of the page), and the light.
    const f32 length = std::sqrt((dx * dx) + (dy * dy) + 1.0F);
    const f32 planar = std::cos(style.light_elevation);
    const f32 across = std::sqrt((style.light_x * style.light_x) + (style.light_y * style.light_y));
    const f32 lx = across > 0.0F ? planar * style.light_x / across : 0.0F;
    const f32 ly = across > 0.0F ? planar * style.light_y / across : 0.0F;
    const f32 lz = std::sin(style.light_elevation);
    const f32 lambert = std::max(0.0F, ((-dx * lx) + (-dy * ly) + lz) / length);
    const f32 flat = lz > 0.0F ? lz : 1.0F;
    return 1.0F + (style.shade_strength * ((lambert / flat) - 1.0F));
}

}  // namespace

f32 HeightGrid::at(f32 x, f32 z) const noexcept {
    if (columns == 0 || rows == 0 || heights.size() < static_cast<usize>(columns) * rows) {
        return 0.0F;
    }
    const f32 u = std::clamp((x - origin_x) / spacing, 0.0F, static_cast<f32>(columns - 1U));
    const f32 v = std::clamp((z - origin_z) / spacing, 0.0F, static_cast<f32>(rows - 1U));
    const u32 c0 = std::min(static_cast<u32>(u), columns - 1U);
    const u32 r0 = std::min(static_cast<u32>(v), rows - 1U);
    const u32 c1 = std::min(c0 + 1U, columns - 1U);
    const u32 r1 = std::min(r0 + 1U, rows - 1U);
    const f32 fu = u - static_cast<f32>(c0);
    const f32 fv = v - static_cast<f32>(r0);
    const auto sample = [&](u32 c, u32 r) noexcept {
        return heights[(static_cast<usize>(r) * columns) + c];
    };
    const f32 top = sample(c0, r0) + ((sample(c1, r0) - sample(c0, r0)) * fu);
    const f32 bottom = sample(c0, r1) + ((sample(c1, r1) - sample(c0, r1)) * fu);
    return top + ((bottom - top) * fv);
}

Vec2 texel_point(const MinimapRegion& region, u32 width, u32 height, u32 column, u32 row) noexcept {
    const f32 u = (static_cast<f32>(column) + 0.5F) / static_cast<f32>(width);
    const f32 v = (static_cast<f32>(row) + 0.5F) / static_cast<f32>(height);
    return Vec2{region.left_x + ((region.right_x - region.left_x) * u),
                region.top_z + ((region.bottom_z - region.top_z) * v)};
}

Status classify_contours(Span<const f32> heights, u32 width, u32 height,
                         const TopographicStyle& style, Span<Contour> out) noexcept {
    if (!sized(width, height, heights.size()) || out.size() != heights.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "minimap: the contour layer is not width * height of the heights");
    }
    if (!(style.contour_interval > 0.0F)) {
        return fail(ErrorCode::InvalidArgument, "minimap: a contour interval is above zero");
    }
    const auto h = [&](u32 c, u32 r) noexcept {
        return heights[(static_cast<usize>(r) * width) + c];
    };
    for (u32 row = 0; row < height; ++row) {
        for (u32 column = 0; column < width; ++column) {
            const f32 here = h(column, row);
            Contour mark = Contour::None;
            const bool wet = here < style.water_level;
            const struct {
                bool inside;
                u32 c;
                u32 r;
            } neighbours[] = {{column > 0, column - 1U, row},
                              {column + 1U < width, column + 1U, row},
                              {row > 0, column, row - 1U},
                              {row + 1U < height, column, row + 1U}};
            for (const auto& neighbour : neighbours) {
                if (!neighbour.inside || wet) {
                    continue;
                }
                const f32 there = h(neighbour.c, neighbour.r);
                if (there < style.water_level) {
                    mark = Contour::Shore;
                    break;
                }
                mark = stronger(mark, crossing(here, there, style));
            }
            out[(static_cast<usize>(row) * width) + column] = mark;
        }
    }
    return ok();
}

Status bake_topographic(const TopographicInput& input, const TopographicStyle& style,
                        Span<u32> out) noexcept {
    const u32 width = input.width;
    const u32 height = input.height;
    if (!sized(width, height, out.size())) {
        return fail(ErrorCode::InvalidArgument, "minimap: the bake is not width * height texels");
    }
    if ((!input.colours.empty() && input.colours.size() != out.size()) ||
        (!input.roads.empty() && input.roads.size() != out.size())) {
        return fail(ErrorCode::InvalidArgument,
                    "minimap: the ground's colours and roads are the image's size or absent");
    }
    Array<f32> heights(system_allocator(MemoryDomain::Renderer));
    Array<Contour> contours(system_allocator(MemoryDomain::Renderer));
    if (!heights.resize(out.size()) || !contours.resize(out.size())) {
        return fail(ErrorCode::OutOfMemory, "minimap: the bake's scratch");
    }
    for (u32 row = 0; row < height; ++row) {
        for (u32 column = 0; column < width; ++column) {
            const Vec2 at = texel_point(input.region, width, height, column, row);
            heights[(static_cast<usize>(row) * width) + column] = input.heights.at(at.x, at.y);
        }
    }
    if (Status classified =
            classify_contours(heights.span(), width, height, style, contours.span());
        !classified) {
        return classified;
    }
    const f32 metres_x =
        std::fabs(input.region.right_x - input.region.left_x) / static_cast<f32>(width);
    const f32 metres_z =
        std::fabs(input.region.bottom_z - input.region.top_z) / static_cast<f32>(height);
    const Rgb ground = unpack_rgb(style.ground_colour);
    const Rgb contour = unpack_rgb(style.contour_colour);
    const Rgb shallow = unpack_rgb(style.shallow_colour);
    const Rgb deep = unpack_rgb(style.deep_colour);
    const Rgb shore = unpack_rgb(style.shore_colour);
    const Rgb road = unpack_rgb(style.road_colour);
    for (u32 row = 0; row < height; ++row) {
        for (u32 column = 0; column < width; ++column) {
            const usize index = (static_cast<usize>(row) * width) + column;
            const f32 h = heights[index];
            Rgb colour = input.colours.empty() ? ground : unpack_texel(input.colours[index]);
            if (h < style.water_level) {
                const f32 depth =
                    std::clamp((style.water_level - h) / style.water_depth, 0.0F, 1.0F);
                out[index] = pack_texel(mix(shallow, deep, depth));
                continue;
            }
            if (!input.roads.empty() && input.roads[index] > 0U) {
                colour = mix(colour, road,
                             style.road_alpha * (static_cast<f32>(input.roads[index]) / 255.0F));
            }
            colour = scale(colour, hillshade(heights.span(), width, height, column, row, metres_x,
                                             metres_z, style));
            switch (contours[index]) {
                case Contour::Line:
                    colour = mix(colour, contour, style.contour_alpha);
                    break;
                case Contour::Index:
                    colour = mix(colour, contour, style.index_alpha);
                    break;
                case Contour::Shore:
                    colour = shore;
                    break;
                case Contour::None:
                    break;
            }
            out[index] = pack_texel(colour);
        }
    }
    return ok();
}

namespace {

/// One texel of the bake through one sample of the field.
[[nodiscard]] u32 fog_texel(u32 base, const fog_of_war::FogSample& sample,
                            const MinimapFogLook& look) noexcept {
    const Rgb seen = unpack_texel(base);
    const f32 luma = (0.2126F * seen.r) + (0.7152F * seen.g) + (0.0722F * seen.b);
    const f32 grey = luma * look.explored_brightness;
    // Black, to the explored grey by how explored, to the bake by how visible.
    const Rgb remembered = scale(Rgb{grey, grey, grey}, sample.explored);
    return pack_texel(mix(remembered, seen, sample.visible));
}

}  // namespace

Status apply_minimap_fog(Span<const u32> base, u32 width, u32 height, const MinimapRegion& region,
                         Span<const u8> texels, u32 grid_width, u32 grid_height,
                         const fog_of_war::FogGridPlacement& placement, const MinimapFogLook& look,
                         Span<u32> out) noexcept {
    if (!sized(width, height, base.size()) || out.size() != base.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "minimap: the fogged image is not the bake's width * height");
    }
    if (texels.size() != static_cast<usize>(grid_width) * grid_height * fog_of_war::kTexelBytes) {
        return fail(ErrorCode::InvalidArgument, "minimap: the field's texels are not its grid's");
    }
    for (u32 row = 0; row < height; ++row) {
        for (u32 column = 0; column < width; ++column) {
            const usize index = (static_cast<usize>(row) * width) + column;
            const Vec2 at = texel_point(region, width, height, column, row);
            out[index] = fog_texel(base[index],
                                   fog_of_war::sample_visibility(texels, grid_width, grid_height,
                                                                 placement, at.x, at.y),
                                   look);
        }
    }
    return ok();
}

void draw_icons(Span<const MinimapIcon> icons, const MinimapRegion& region, u32 width, u32 height,
                Span<u32> image) noexcept {
    if (!sized(width, height, image.size())) {
        return;
    }
    const f32 span_x = region.right_x - region.left_x;
    const f32 span_z = region.bottom_z - region.top_z;
    if (span_x == 0.0F || span_z == 0.0F) {
        return;
    }
    const Rgb edge{18.0F, 14.0F, 10.0F};
    for (const MinimapIcon& icon : icons) {
        const f32 cx = ((icon.x - region.left_x) / span_x) * static_cast<f32>(width);
        const f32 cy = ((icon.z - region.top_z) / span_z) * static_cast<f32>(height);
        if (cx < 0.0F || cy < 0.0F || cx >= static_cast<f32>(width) ||
            cy >= static_cast<f32>(height)) {
            continue;
        }
        const f32 reach = std::max(icon.radius, 0.5F);
        const Rgb fill = unpack_rgb(icon.colour);
        const auto first_column = static_cast<i32>(std::floor(cx - reach));
        const auto last_column = static_cast<i32>(std::ceil(cx + reach));
        const auto first_row = static_cast<i32>(std::floor(cy - reach));
        const auto last_row = static_cast<i32>(std::ceil(cy + reach));
        for (i32 row = std::max(first_row, 0); row <= last_row && row < static_cast<i32>(height);
             ++row) {
            for (i32 column = std::max(first_column, 0);
                 column <= last_column && column < static_cast<i32>(width); ++column) {
                const f32 dx = std::fabs((static_cast<f32>(column) + 0.5F) - cx);
                const f32 dy = std::fabs((static_cast<f32>(row) + 0.5F) - cy);
                const f32 distance =
                    icon.shape == IconShape::Square ? std::max(dx, dy) : std::hypot(dx, dy);
                if (distance > reach) {
                    continue;
                }
                const bool rim = icon.outline && distance > reach - 1.0F;
                image[(static_cast<usize>(row) * width) + static_cast<usize>(column)] =
                    pack_texel(rim ? edge : fill);
            }
        }
    }
}

TopographicMinimap::TopographicMinimap(Allocator& allocator) noexcept
    : base_(allocator),
      fogged_(allocator),
      image_(allocator),
      field_(allocator),
      dirty_(allocator) {}

Status TopographicMinimap::bake(const TopographicInput& input,
                                const TopographicStyle& style) noexcept {
    const usize count = static_cast<usize>(input.width) * input.height;
    if (!base_.resize(count) || !fogged_.resize(count) || !image_.resize(count)) {
        return fail(ErrorCode::OutOfMemory, "minimap: the images");
    }
    if (Status baked = bake_topographic(input, style, base_.span()); !baked) {
        return baked;
    }
    region_ = input.region;
    width_ = input.width;
    height_ = input.height;
    fogged_once_ = false;
    return ok();
}

Status TopographicMinimap::compose(Span<const u8> texels, u32 grid_width, u32 grid_height,
                                   const fog_of_war::FogGridPlacement& placement,
                                   Span<const MinimapIcon> icons) noexcept {
    if (width_ == 0) {
        return fail(ErrorCode::InvalidArgument, "minimap: compose before bake");
    }
    if (texels.size() != static_cast<usize>(grid_width) * grid_height * fog_of_war::kTexelBytes) {
        return fail(ErrorCode::InvalidArgument, "minimap: the field's texels are not its grid's");
    }
    const bool same_grid = fogged_once_ && grid_width == grid_width_ &&
                           grid_height == grid_height_ && field_.size() == texels.size();
    if (!same_grid) {
        if (Status fogged =
                apply_minimap_fog(base_.span(), width_, height_, region_, texels, grid_width,
                                  grid_height, placement, look, fogged_.span());
            !fogged) {
            return fogged;
        }
        if (Status kept = field_.resize(texels.size()); !kept) {
            return kept;
        }
        grid_width_ = grid_width;
        grid_height_ = grid_height;
        fogged_once_ = true;
        ++fog_passes_;
        refogged_ += static_cast<u64>(width_) * height_;
    } else if (Status updated = refog_changed(texels, placement); !updated) {
        return updated;
    }
    std::memcpy(field_.data(), texels.data(), texels.size());
    std::copy(fogged_.begin(), fogged_.end(), image_.begin());
    draw_icons(icons, region_, width_, height_, image_.span());
    return ok();
}

Status TopographicMinimap::refog_changed(Span<const u8> texels,
                                         const fog_of_war::FogGridPlacement& placement) noexcept {
    // THE CELLS THAT CHANGED, and their neighbours: a texel's sample reads the cells half a cell
    // round its point, so a texel in a cell beside a changed one may have changed too.
    const u32 cells = grid_width_ * grid_height_;
    if (Status sized = dirty_.resize(cells); !sized) {
        return sized;
    }
    std::fill(dirty_.begin(), dirty_.end(), u8{0});
    u32 changed = 0;
    for (u32 cell = 0; cell < cells; ++cell) {
        if (std::memcmp(texels.data() + (static_cast<usize>(cell) * fog_of_war::kTexelBytes),
                        field_.data() + (static_cast<usize>(cell) * fog_of_war::kTexelBytes),
                        fog_of_war::kTexelBytes) == 0) {
            continue;
        }
        ++changed;
        const u32 cx = cell % grid_width_;
        const u32 cz = cell / grid_width_;
        for (u32 z = cz > 0 ? cz - 1U : 0U; z <= cz + 1U && z < grid_height_; ++z) {
            for (u32 x = cx > 0 ? cx - 1U : 0U; x <= cx + 1U && x < grid_width_; ++x) {
                dirty_[(static_cast<usize>(z) * grid_width_) + x] = 1U;
            }
        }
    }
    if (changed == 0) {
        ++reused_;
        return ok();
    }
    ++fog_passes_;
    for (u32 row = 0; row < height_; ++row) {
        for (u32 column = 0; column < width_; ++column) {
            const Vec2 at = texel_point(region_, width_, height_, column, row);
            const f32 fx = (at.x - placement.origin_x) / placement.cell_size;
            const f32 fz = (at.y - placement.origin_z) / placement.cell_size;
            // Off the grid is never explored, whatever the field holds: nothing there changes.
            if (fx < 0.0F || fz < 0.0F || fx >= static_cast<f32>(grid_width_) ||
                fz >= static_cast<f32>(grid_height_)) {
                continue;
            }
            const usize cell = (static_cast<usize>(fz) * grid_width_) + static_cast<usize>(fx);
            if (dirty_[cell] == 0U) {
                continue;
            }
            const usize index = (static_cast<usize>(row) * width_) + column;
            fogged_[index] =
                fog_texel(base_[index],
                          fog_of_war::sample_visibility(texels, grid_width_, grid_height_,
                                                        placement, at.x, at.y),
                          look);
            ++refogged_;
        }
    }
    return ok();
}

}  // namespace cy::rendering::minimap
