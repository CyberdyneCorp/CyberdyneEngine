// SPDX-License-Identifier: MIT
#pragma once
// Shared lightmap atlases: every static object of a level placed in one set of pages, and the
// one-word address a draw carries to find its rectangle.
//
// `rendering-global-illumination` — "UV2 and chart packing": "Charts SHALL be packed into atlases
// with padding sufficient for bilinear filtering and mip generation at the bake resolution", and
// "Lightmap baking": "per-object lightmap resolution scaling, a global texel density".
//
// ================================================================================================
// ONE RECTANGLE PER OBJECT, AND THE UNWRAP INSIDE IT
// ================================================================================================
//
// The importer unwraps each mesh into its own unit square (`cy::import::generate_uv2`, over
// xatlas), with padding between that mesh's charts. A level places many instances of those meshes,
// so what is packed here is one RECTANGLE per placed object, sized from the object's world area,
// the global texel density and the object's own resolution scale; the mesh's UV2 square is mapped
// into the rectangle's interior by a scale and an offset. That keeps one mesh's UV2 stream shared
// by every instance of it — the frame reads the cooked `TexCoords2` stream unchanged and applies
// the instance's rectangle — which is the arrangement that lets a thousand rocks share one vertex
// buffer and still own a thousand regions of the atlas.
//
// ================================================================================================
// PADDING FOR BILINEAR AND FOR THE MIP CHAIN
// ================================================================================================
//
// Rectangles start and end on a BLOCK grid, a block being `page_size / kAddressBlocks` texels, so a
// rectangle boundary stays on a texel boundary down to mip level log2(block). Around each
// rectangle's interior is a GUTTER of `gutter_texels` on every side, at least two texels and at
// least one texel of the coarsest mip level the padding is declared for: a bilinear tap at the
// interior's edge, at any of those levels, reads the gutter — which the bake dilates with the
// object's own border — and never a neighbour.
//
// ================================================================================================
// THE ADDRESS IS ONE WORD, AND THAT IS WHAT THE DRAW LIST ALREADY CARRIES
// ================================================================================================
//
// `GpuDrawInstance::gi_address` is "lightmap or GI volume addressing, opaque to" the forward
// module. A rectangle on the block grid is four seven-bit numbers and a page, which fits in it
// exactly:
//
//     bits  0..6   block x        bits 14..20  block width - 1
//     bits  7..13  block y        bits 21..27  block height - 1
//     bits 28..31  page + 1 — ZERO MEANS "NOT LIGHTMAPPED", which is the field's default
//
// so no draw needs a second record and no shader needs a second buffer to find its lightmap.
// `cy/frame.slang`'s `lightmapCoordinate` decodes it with the same arithmetic as
// `atlas_coordinate` below, which is the CPU reference the frame is held to.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>

namespace cy::rendering::lightmap_bake {

/// Blocks per page axis. Seven bits in the address.
inline constexpr u32 kAddressBlocks = 128;
/// Pages one address can name. Four bits, with zero reserved for "not lightmapped".
inline constexpr u32 kMaxPages = 15;
/// The value of `gi_address` a draw without a lightmap carries.
inline constexpr u32 kNoLightmapAddress = 0;

struct AtlasSettings {
    /// Texels along one side of a page. A power of two in [128, 4096], so a block is a whole power
    /// of two of texels.
    u32 page_size = 512;
    /// Texels per metre, for the whole level. One number describes a project; an object's own
    /// `resolution_scale` multiplies it.
    f32 texel_density = 8.0F;
    /// The mip levels below the base the padding must survive. Clamped to log2(block), because a
    /// rectangle boundary finer than that stops being a texel boundary.
    u32 mip_levels = 2;
    /// Pages the packer may open before it refuses the level.
    u32 max_pages = kMaxPages;
};

/// One placed object as the packer sees it.
struct AtlasObject {
    /// The object's surface area in the world, in square metres, after its transform.
    f32 surface_area = 1.0F;
    /// The share of its UV2 square its charts cover, in (0, 1]: `Uv2Report::utilisation`. The
    /// rectangle is enlarged by its inverse so the CHARTS, not the square, get the density.
    f32 uv_coverage = 1.0F;
    /// Width over height of the UV2 square's texel grid (`Uv2Report::width / height`).
    f32 aspect = 1.0F;
    /// The object's resolution scale over the global density.
    f32 resolution_scale = 1.0F;
};

/// Where one object landed. Everything in blocks.
struct AtlasPlacement {
    u32 page = 0;
    u32 block_x = 0;
    u32 block_y = 0;
    u32 block_width = 0;
    u32 block_height = 0;
    /// True when the object asked for more than a page and was given a page.
    bool clamped = false;
};

/// The packed level.
struct AtlasLayout {
    AtlasSettings settings{};
    u32 pages = 0;
    u32 block_texels = 0;
    u32 gutter_texels = 0;
    /// The mip levels the padding actually protects, after the clamp.
    u32 mip_levels = 0;
    Array<AtlasPlacement> placements;
    /// Texels inside some rectangle over texels in all pages.
    f32 occupancy = 0.0F;
    u32 clamped_objects = 0;
};

/// Pack every object into shared pages.
///
/// Deterministic: objects are placed tallest first with the input index as the tie-break, on
/// shelves filled left to right, so one level packs to one layout on every machine. Fails with
/// `InvalidArgument` on a page size outside its range and with `OutOfRange` when the level needs
/// more than `max_pages`.
[[nodiscard]] Status pack_atlas(Span<const AtlasObject> objects, const AtlasSettings& settings,
                                AtlasLayout& out) noexcept;

/// The gutter a layout with these settings leaves on every side of a rectangle's interior.
[[nodiscard]] u32 gutter_for(const AtlasSettings& settings) noexcept;

/// The address word of a placement: the `gi_address` its draws carry.
[[nodiscard]] u32 encode_address(const AtlasPlacement& placement) noexcept;
/// The placement an address names, or false for `kNoLightmapAddress` and for a malformed word.
[[nodiscard]] bool decode_address(u32 address, AtlasPlacement& out) noexcept;

/// Where the atlas stores the texel a UV2 coordinate names, in texels of the whole stacked atlas:
/// x in [0, page_size), y in [0, page_size * pages). The interior of the rectangle, gutter
/// excluded, spans the UV2 unit square.
///
/// THE FORMULA `cy/frame.slang`'s `lightmapCoordinate` TRANSCRIBES. Kept in one function so the
/// bake's rasteriser, the CPU reference sampler and the test that holds the frame to it cannot
/// disagree about where a coordinate lands.
[[nodiscard]] Vec2 atlas_coordinate(u32 address, Vec2 uv2, u32 page_size,
                                    u32 gutter_texels) noexcept;

}  // namespace cy::rendering::lightmap_bake
