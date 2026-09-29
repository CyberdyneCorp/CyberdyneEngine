// SPDX-License-Identifier: MIT
// The lightmap's mip chain, filtered and dilated per chart. See mips.h.

#include <cy/rendering/lightmap_bake/asset.h>
#include <cy/rendering/lightmap_bake/bake.h>
#include <cy/rendering/lightmap_bake/mips.h>

#include <bit>

namespace cy::rendering::lightmap_bake {
namespace {

/// One rectangle in texels of the whole stacked atlas at the base level, half-open.
struct Rect {
    u32 x0 = 0;
    u32 y0 = 0;
    u32 x1 = 0;
    u32 y1 = 0;

    [[nodiscard]] Rect at(u32 level) const noexcept {
        return Rect{x0 >> level, y0 >> level, x1 >> level, y1 >> level};
    }
    [[nodiscard]] bool contains(i64 x, i64 y) const noexcept {
        return x >= i64{x0} && x < i64{x1} && y >= i64{y0} && y < i64{y1};
    }
};

[[nodiscard]] Status rectangles_of(const BakedLightmap& lightmap, Array<Rect>& out) noexcept {
    out.clear();
    const u32 block = lightmap.page_size / kAddressBlocks;
    for (const u32 address : lightmap.addresses) {
        AtlasPlacement placement;
        if (!decode_address(address, placement)) {
            continue;
        }
        Rect rect;
        rect.x0 = placement.block_x * block;
        rect.y0 = (placement.page * lightmap.page_size) + (placement.block_y * block);
        rect.x1 = rect.x0 + (placement.block_width * block);
        rect.y1 = rect.y0 + (placement.block_height * block);
        if (Status pushed = out.push_back(rect); !pushed) {
            return pushed;
        }
    }
    return ok();
}

/// The chart nearest a texel inside its rectangle and how far away it is, in base texels
/// (Chebyshev: one ring of the base dilation per step). `kNoChart` outside every rectangle and in
/// a rectangle no chart covers.
struct Nearest {
    u32 chart = kNoChart;
    u32 distance = ~0U;

    [[nodiscard]] bool before(const Nearest& other) const noexcept {
        return distance < other.distance || (distance == other.distance && chart < other.chart);
    }
};

/// The fields filtered together at one level: every plane of the texels, and the shadow mask.
struct Layer {
    LightmapTexels* texels = nullptr;
    u32 plane = 0;
};

/// One level as the builder sees it.
struct Level {
    u32 width = 0;
    u32 height = 0;
    Layer layers[4];
    u32 layer_count = 0;
    /// The chart each texel's value was filtered from, `kNoChart` for padding: what the next level
    /// down is built from.
    Array<u32> genuine;
    /// Which chart owns each texel: its genuine chart, or for padding the nearest one.
    Array<Nearest> nearest;
    /// Whether the texel holds its owner's value yet: every genuine texel, and padding as the
    /// dilation reaches it.
    Array<u8> filled;

    [[nodiscard]] usize index(u32 x, u32 y) const noexcept { return (usize{y} * width) + x; }
    [[nodiscard]] Vec4& value(u32 layer, u32 x, u32 y) noexcept {
        return layers[layer].texels->texels[layers[layer].texels->index(layers[layer].plane, x, y)];
    }
};

void add_layers(Level& level, LightmapTexels& texels) noexcept {
    for (u32 plane = 0; plane < texels.planes && level.layer_count < 4U; ++plane) {
        level.layers[level.layer_count++] = Layer{&texels, plane};
    }
}

[[nodiscard]] Status sized_like(LightmapTexels& out, const LightmapTexels& base,
                                u32 level) noexcept {
    out.width = base.width >> level;
    out.height = base.height >> level;
    out.planes = base.planes;
    out.texels.clear();
    return out.texels.resize(usize{out.planes} * out.width * out.height);
}

// --- The base level's ownership ------------------------------------------------------------------

/// One ring outward from every owned texel of `rect`. Returns whether it owned anything new.
[[nodiscard]] bool grow_ring(Level& level, const Rect& rect, u32 ring) noexcept {
    bool grew = false;
    for (u32 y = rect.y0; y < rect.y1; ++y) {
        for (u32 x = rect.x0; x < rect.x1; ++x) {
            if (level.nearest[level.index(x, y)].chart != kNoChart) {
                continue;
            }
            Nearest best;
            for (i64 dy = -1; dy <= 1; ++dy) {
                for (i64 dx = -1; dx <= 1; ++dx) {
                    const i64 nx = i64{x} + dx;
                    const i64 ny = i64{y} + dy;
                    if (!rect.contains(nx, ny)) {
                        continue;
                    }
                    const Nearest& other =
                        level.nearest[level.index(static_cast<u32>(nx), static_cast<u32>(ny))];
                    if (other.distance < ring && other.before(best)) {
                        best = Nearest{other.chart, ring};
                    }
                }
            }
            if (best.chart != kNoChart) {
                level.nearest[level.index(x, y)] = best;
                grew = true;
            }
        }
    }
    return grew;
}

/// Every base texel of every rectangle owned by its nearest chart.
void own_base(Level& base, Span<const Rect> rects) noexcept {
    for (usize index = 0; index < base.genuine.size(); ++index) {
        base.nearest[index] =
            base.genuine[index] == kNoChart ? Nearest{} : Nearest{base.genuine[index], 0U};
    }
    for (const Rect& rect : rects) {
        for (u32 ring = 1; grow_ring(base, rect, ring); ++ring) {
        }
    }
}

// --- One level from the one above ----------------------------------------------------------------

/// The chart most of a coarse texel's four fine texels belong to, lowest id on a tie, or
/// `kNoChart` when none does.
[[nodiscard]] u32 dominant_chart(const u32 (&charts)[4]) noexcept {
    u32 best = kNoChart;
    u32 best_count = 0;
    for (const u32 candidate : charts) {
        if (candidate == kNoChart) {
            continue;
        }
        u32 count = 0;
        for (const u32 other : charts) {
            count += other == candidate ? 1U : 0U;
        }
        if (count > best_count || (count == best_count && candidate < best)) {
            best = candidate;
            best_count = count;
        }
    }
    return best;
}

/// One coarse texel from its four fine texels: the mean of the dominant chart's, or of all four
/// when no chart covers any of them — a value the dilation then replaces inside a rectangle.
void filter_texel(Level& fine, Level& coarse, u32 x, u32 y) noexcept {
    const u32 fx[4] = {2U * x, (2U * x) + 1U, 2U * x, (2U * x) + 1U};
    const u32 fy[4] = {2U * y, 2U * y, (2U * y) + 1U, (2U * y) + 1U};
    u32 charts[4] = {};
    Nearest owner;
    for (u32 tap = 0; tap < 4U; ++tap) {
        charts[tap] = fine.genuine[fine.index(fx[tap], fy[tap])];
        const Nearest& near = fine.nearest[fine.index(fx[tap], fy[tap])];
        owner = near.before(owner) ? near : owner;
    }
    const u32 chart = dominant_chart(charts);
    for (u32 layer = 0; layer < coarse.layer_count; ++layer) {
        Vec4 sum{0.0F, 0.0F, 0.0F, 0.0F};
        f32 taken = 0.0F;
        for (u32 tap = 0; tap < 4U; ++tap) {
            if (charts[tap] == chart) {
                sum = sum + fine.value(layer, fx[tap], fy[tap]);
                taken += 1.0F;
            }
        }
        coarse.value(layer, x, y) = sum * (1.0F / taken);
    }
    const usize at = coarse.index(x, y);
    coarse.genuine[at] = chart;
    coarse.nearest[at] = chart == kNoChart ? owner : Nearest{chart, 0U};
    coarse.filled[at] = chart == kNoChart ? 0U : 1U;
}

void filter_level(Level& fine, Level& coarse) noexcept {
    for (u32 y = 0; y < coarse.height; ++y) {
        for (u32 x = 0; x < coarse.width; ++x) {
            filter_texel(fine, coarse, x, y);
        }
    }
}

/// Fill one padding texel from the neighbours its owner has already filled in `snapshot`.
/// Returns false when none has been.
[[nodiscard]] bool fill_texel(Level& level, const Array<u8>& snapshot, const Rect& rect, u32 x,
                              u32 y) noexcept {
    const u32 owner = level.nearest[level.index(x, y)].chart;
    Vec4 sums[4] = {};
    f32 taken = 0.0F;
    for (i64 dy = -1; dy <= 1; ++dy) {
        for (i64 dx = -1; dx <= 1; ++dx) {
            const i64 nx = i64{x} + dx;
            const i64 ny = i64{y} + dy;
            if (!rect.contains(nx, ny)) {
                continue;
            }
            const auto ux = static_cast<u32>(nx);
            const auto uy = static_cast<u32>(ny);
            if (snapshot[level.index(ux, uy)] == 0U ||
                level.nearest[level.index(ux, uy)].chart != owner) {
                continue;
            }
            for (u32 layer = 0; layer < level.layer_count; ++layer) {
                sums[layer] = sums[layer] + level.value(layer, ux, uy);
            }
            taken += 1.0F;
        }
    }
    if (taken == 0.0F) {
        return false;
    }
    for (u32 layer = 0; layer < level.layer_count; ++layer) {
        level.value(layer, x, y) = sums[layer] * (1.0F / taken);
    }
    level.filled[level.index(x, y)] = 1U;
    return true;
}

/// One ring of dilation inside `rect`: every padding texel with a filled neighbour of its own
/// chart. Reads `snapshot`, so a pass fills one ring whatever order it walks in.
[[nodiscard]] u32 dilate_ring(Level& level, Array<u8>& snapshot, const Rect& rect) noexcept {
    for (u32 y = rect.y0; y < rect.y1; ++y) {
        for (u32 x = rect.x0; x < rect.x1; ++x) {
            snapshot[level.index(x, y)] = level.filled[level.index(x, y)];
        }
    }
    u32 filled = 0;
    for (u32 y = rect.y0; y < rect.y1; ++y) {
        for (u32 x = rect.x0; x < rect.x1; ++x) {
            const usize at = level.index(x, y);
            if (snapshot[at] == 0U && level.nearest[at].chart != kNoChart &&
                fill_texel(level, snapshot, rect, x, y)) {
                filled += 1U;
            }
        }
    }
    return filled;
}

void dilate_level(Level& level, Span<const Rect> rects, u32 depth, Array<u8>& snapshot) noexcept {
    for (const Rect& base : rects) {
        const Rect rect = base.at(depth);
        while (dilate_ring(level, snapshot, rect) != 0U) {
        }
    }
}

/// Every value of the level through half precision, as the base was: the device and the cooked
/// asset hold halves, and the next level down is filtered from what they hold.
void round_level(Level& level) noexcept {
    const auto rounded = [](f32 value) { return float_from_half(half_from_float(value)); };
    for (u32 layer = 0; layer < level.layer_count; ++layer) {
        for (u32 y = 0; y < level.height; ++y) {
            for (u32 x = 0; x < level.width; ++x) {
                Vec4& value = level.value(layer, x, y);
                value =
                    Vec4{rounded(value.x), rounded(value.y), rounded(value.z), rounded(value.w)};
            }
        }
    }
}

/// The level's bookkeeping, and its layers pointed at the lightmap's own storage for it.
[[nodiscard]] Status open_level(BakedLightmap& lightmap, u32 depth, Level& out) noexcept {
    const bool masked = !lightmap.shadow_mask.texels.empty();
    LightmapTexels& texels = depth == 0 ? lightmap.texels : lightmap.mip_texels[depth - 1U];
    LightmapTexels& mask = depth == 0 ? lightmap.shadow_mask : lightmap.mip_shadow_mask[depth - 1U];
    if (depth != 0) {
        if (Status sized = sized_like(texels, lightmap.texels, depth); !sized) {
            return sized;
        }
        if (masked) {
            if (Status sized = sized_like(mask, lightmap.shadow_mask, depth); !sized) {
                return sized;
            }
        }
    }
    out.width = lightmap.texels.width >> depth;
    out.height = lightmap.texels.height >> depth;
    out.layer_count = 0;
    add_layers(out, texels);
    if (masked) {
        add_layers(out, mask);
    }
    const usize count = usize{out.width} * out.height;
    if (Status sized = out.genuine.resize(count); !sized) {
        return sized;
    }
    if (Status sized = out.filled.resize(count); !sized) {
        return sized;
    }
    return out.nearest.resize(count);
}

[[nodiscard]] Status check(const BakedLightmap& lightmap, Span<const u32> charts) noexcept {
    const LightmapTexels& texels = lightmap.texels;
    const u32 block = lightmap.page_size / kAddressBlocks;
    const u32 block_levels = block == 0 ? 0U : static_cast<u32>(std::countr_zero(block));
    if (charts.size() != usize{texels.width} * texels.height) {
        return fail(ErrorCode::InvalidArgument, "a lightmap's mip chain needs one chart per texel");
    }
    if (lightmap.mip_levels > block_levels || lightmap.mip_levels > kMaxLightmapMipLevels) {
        return fail(ErrorCode::InvalidArgument,
                    "a lightmap's mip levels go below the block its rectangles are aligned to");
    }
    const bool mask_ok =
        lightmap.shadow_mask.texels.empty() || (lightmap.shadow_mask.width == texels.width &&
                                                lightmap.shadow_mask.height == texels.height);
    if (!mask_ok) {
        return fail(ErrorCode::InvalidArgument,
                    "a lightmap's shadow mask is not laid out like its texels");
    }
    return ok();
}

}  // namespace

Status build_lightmap_mips(BakedLightmap& lightmap, Span<const u32> charts) noexcept {
    for (u32 level = 0; level < kMaxLightmapMipLevels; ++level) {
        lightmap.mip_texels[level] = LightmapTexels();
        lightmap.mip_shadow_mask[level] = LightmapTexels();
    }
    if (lightmap.mip_levels == 0) {
        return ok();
    }
    if (Status valid = check(lightmap, charts); !valid) {
        return valid;
    }
    Array<Rect> rects;
    if (Status found = rectangles_of(lightmap, rects); !found) {
        return found;
    }
    Level fine;
    if (Status opened = open_level(lightmap, 0, fine); !opened) {
        return opened;
    }
    for (usize index = 0; index < charts.size(); ++index) {
        fine.genuine[index] = charts[index];
    }
    own_base(fine, rects.span());
    Array<u8> snapshot;
    for (u32 depth = 1; depth <= lightmap.mip_levels; ++depth) {
        Level coarse;
        if (Status opened = open_level(lightmap, depth, coarse); !opened) {
            return opened;
        }
        if (Status sized = snapshot.resize(usize{coarse.width} * coarse.height); !sized) {
            return sized;
        }
        filter_level(fine, coarse);
        dilate_level(coarse, rects.span(), depth, snapshot);
        round_level(coarse);
        fine = std::move(coarse);
    }
    return ok();
}

const LightmapTexels& lightmap_level(const BakedLightmap& lightmap, u32 level) noexcept {
    if (level == 0 || level > lightmap.mip_levels || level > kMaxLightmapMipLevels) {
        return lightmap.texels;
    }
    return lightmap.mip_texels[level - 1U];
}

const LightmapTexels& shadow_mask_level(const BakedLightmap& lightmap, u32 level) noexcept {
    if (level == 0 || level > lightmap.mip_levels || level > kMaxLightmapMipLevels) {
        return lightmap.shadow_mask;
    }
    return lightmap.mip_shadow_mask[level - 1U];
}

}  // namespace cy::rendering::lightmap_bake
