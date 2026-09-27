// SPDX-License-Identifier: MIT
// Shared lightmap atlases. See atlas.h.

#include <cy/rendering/lightmap_bake/atlas.h>

#include <algorithm>
#include <bit>
#include <cmath>

namespace cy::rendering::lightmap_bake {
namespace {

constexpr u32 kFieldMask = kAddressBlocks - 1U;

[[nodiscard]] bool page_size_valid(u32 page_size) noexcept {
    return page_size >= kAddressBlocks && page_size <= 4096U && std::has_single_bit(page_size);
}

/// The texel size of an object's rectangle, gutter included, in blocks.
[[nodiscard]] AtlasPlacement size_of(const AtlasObject& object, const AtlasSettings& settings,
                                     u32 block_texels, u32 gutter) noexcept {
    const f32 density = settings.texel_density * std::max(object.resolution_scale, 0.0F);
    const f32 coverage = std::clamp(object.uv_coverage, 0.05F, 1.0F);
    const f32 texels = std::max(object.surface_area, 0.0F) * density * density / coverage;
    const f32 aspect = std::clamp(object.aspect, 1.0F / 16.0F, 16.0F);
    const f32 interior_width = std::sqrt(texels * aspect);
    const f32 interior_height = interior_width > 0.0F ? texels / interior_width : 0.0F;

    const auto blocks = [&](f32 interior) {
        const f32 total = std::ceil(interior) + static_cast<f32>(2U * gutter);
        const auto wanted = static_cast<u32>(std::ceil(total / static_cast<f32>(block_texels)));
        // At least one texel of interior beyond the two gutters, whatever the area asked for.
        const u32 least = ((2U * gutter) / block_texels) + 1U;
        return std::max(wanted, least);
    };

    AtlasPlacement placement;
    placement.block_width = blocks(interior_width);
    placement.block_height = blocks(interior_height);
    if (placement.block_width > kAddressBlocks || placement.block_height > kAddressBlocks) {
        placement.clamped = true;
        placement.block_width = std::min(placement.block_width, kAddressBlocks);
        placement.block_height = std::min(placement.block_height, kAddressBlocks);
    }
    return placement;
}

struct Shelf {
    u32 page = 0;
    u32 y = 0;
    u32 height = 0;
    u32 cursor = 0;
};

/// The shelf packer's state: every shelf opened, and how far down each page is used.
class ShelfPacker {
public:
    explicit ShelfPacker(u32 max_pages) noexcept : max_pages_(max_pages) {}

    [[nodiscard]] Status place(AtlasPlacement& placement) noexcept {
        for (Shelf& shelf : shelves_) {
            if (placement.block_height <= shelf.height &&
                shelf.cursor + placement.block_width <= kAddressBlocks) {
                return put(shelf, placement);
            }
        }
        for (u32 page = 0; page < pages_; ++page) {
            if (page_used_[page] + placement.block_height <= kAddressBlocks) {
                return open_shelf(page, placement);
            }
        }
        if (pages_ == max_pages_) {
            return fail(ErrorCode::OutOfRange,
                        "the level's lightmaps need more pages than the atlas may open; lower the "
                        "texel density or raise the page size");
        }
        page_used_[pages_] = 0;
        pages_ += 1;
        return open_shelf(pages_ - 1U, placement);
    }

    [[nodiscard]] u32 pages() const noexcept { return pages_; }

private:
    [[nodiscard]] Status open_shelf(u32 page, AtlasPlacement& placement) noexcept {
        Shelf shelf;
        shelf.page = page;
        shelf.y = page_used_[page];
        shelf.height = placement.block_height;
        page_used_[page] += placement.block_height;
        if (Status pushed = shelves_.push_back(shelf); !pushed) {
            return pushed;
        }
        return put(shelves_.back(), placement);
    }

    static Status put(Shelf& shelf, AtlasPlacement& placement) noexcept {
        placement.page = shelf.page;
        placement.block_x = shelf.cursor;
        placement.block_y = shelf.y;
        shelf.cursor += placement.block_width;
        return ok();
    }

    Array<Shelf> shelves_;
    u32 page_used_[kMaxPages] = {};
    u32 pages_ = 0;
    u32 max_pages_ = kMaxPages;
};

}  // namespace

u32 gutter_for(const AtlasSettings& settings) noexcept {
    const u32 block = page_size_valid(settings.page_size) ? settings.page_size / kAddressBlocks : 1U;
    const auto block_levels = static_cast<u32>(std::countr_zero(block));
    const u32 levels = std::min(settings.mip_levels, block_levels);
    return std::max(2U, 1U << levels);
}

Status pack_atlas(Span<const AtlasObject> objects, const AtlasSettings& settings,
                  AtlasLayout& out) noexcept {
    if (!page_size_valid(settings.page_size)) {
        return fail(ErrorCode::InvalidArgument,
                    "a lightmap page is a power of two between 128 and 4096 texels");
    }
    if (!(settings.texel_density > 0.0F)) {
        return fail(ErrorCode::InvalidArgument, "a lightmap texel density must be positive");
    }
    out.settings = settings;
    out.block_texels = settings.page_size / kAddressBlocks;
    out.gutter_texels = gutter_for(settings);
    out.mip_levels = std::min(settings.mip_levels,
                              static_cast<u32>(std::countr_zero(out.block_texels)));
    out.placements.clear();
    out.clamped_objects = 0;
    if (Status sized = out.placements.resize(objects.size()); !sized) {
        return sized;
    }

    Array<u32> order;
    if (Status sized = order.resize(objects.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < objects.size(); ++index) {
        out.placements[index] =
            size_of(objects[index], settings, out.block_texels, out.gutter_texels);
        order[index] = static_cast<u32>(index);
    }
    std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        return out.placements[a].block_height > out.placements[b].block_height;
    });

    ShelfPacker packer(std::clamp(settings.max_pages, 1U, kMaxPages));
    u64 used_blocks = 0;
    for (const u32 index : order) {
        AtlasPlacement& placement = out.placements[index];
        if (Status placed = packer.place(placement); !placed) {
            return placed;
        }
        used_blocks += u64{placement.block_width} * placement.block_height;
        out.clamped_objects += placement.clamped ? 1U : 0U;
    }
    out.pages = packer.pages();
    const u64 page_blocks = u64{kAddressBlocks} * kAddressBlocks * std::max(out.pages, 1U);
    out.occupancy = static_cast<f32>(used_blocks) / static_cast<f32>(page_blocks);
    return ok();
}

u32 encode_address(const AtlasPlacement& placement) noexcept {
    return (placement.block_x & kFieldMask) | ((placement.block_y & kFieldMask) << 7U) |
           (((placement.block_width - 1U) & kFieldMask) << 14U) |
           (((placement.block_height - 1U) & kFieldMask) << 21U) |
           (((placement.page + 1U) & 0xFU) << 28U);
}

bool decode_address(u32 address, AtlasPlacement& out) noexcept {
    const u32 page = address >> 28U;
    if (page == 0U) {
        return false;
    }
    out.page = page - 1U;
    out.block_x = address & kFieldMask;
    out.block_y = (address >> 7U) & kFieldMask;
    out.block_width = ((address >> 14U) & kFieldMask) + 1U;
    out.block_height = ((address >> 21U) & kFieldMask) + 1U;
    out.clamped = false;
    return out.block_x + out.block_width <= kAddressBlocks &&
           out.block_y + out.block_height <= kAddressBlocks;
}

Vec2 atlas_coordinate(u32 address, Vec2 uv2, u32 page_size, u32 gutter_texels) noexcept {
    AtlasPlacement placement;
    if (!decode_address(address, placement)) {
        return Vec2{0.0F, 0.0F};
    }
    const auto block = static_cast<f32>(page_size / kAddressBlocks);
    const auto gutter = static_cast<f32>(gutter_texels);
    const Vec2 origin{(static_cast<f32>(placement.block_x) * block) + gutter,
                      (static_cast<f32>(placement.block_y) * block) + gutter};
    const Vec2 size{(static_cast<f32>(placement.block_width) * block) - (2.0F * gutter),
                    (static_cast<f32>(placement.block_height) * block) - (2.0F * gutter)};
    const f32 page_offset = static_cast<f32>(placement.page) * static_cast<f32>(page_size);
    return Vec2{origin.x + (uv2.x * size.x), page_offset + origin.y + (uv2.y * size.y)};
}

}  // namespace cy::rendering::lightmap_bake
