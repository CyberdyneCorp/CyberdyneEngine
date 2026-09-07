#include <cy/servers/render/virtual_texturing/address.h>

namespace cy::render::vt {

const char* residency_model_name(ResidencyModel model) noexcept {
    switch (model) {
        case ResidencyModel::Resident:
            return "resident";
        case ResidencyModel::StreamedMip:
            return "streamed-mip";
        case ResidencyModel::VirtualStreamed:
            return "virtual-streamed";
        case ResidencyModel::VirtualRuntime:
            return "virtual-runtime";
        case ResidencyModel::Count:
            break;
    }
    return "unknown";
}

const char* texture_semantic_name(TextureSemantic semantic) noexcept {
    switch (semantic) {
        case TextureSemantic::Colour:
            return "colour";
        case TextureSemantic::Normal:
            return "normal";
        case TextureSemantic::Mask:
            return "mask";
        case TextureSemantic::Height:
            return "height";
        case TextureSemantic::Data:
            return "data";
        case TextureSemantic::HighDynamicRange:
            return "hdr";
        case TextureSemantic::UserInterface:
            return "ui";
        case TextureSemantic::Count:
            break;
    }
    return "unknown";
}

const char* format_class_name(FormatClass klass) noexcept {
    switch (klass) {
        case FormatClass::BlockColour:
            return "block-colour";
        case FormatClass::TwoChannelNormal:
            return "two-channel-normal";
        case FormatClass::SingleChannelMask:
            return "single-channel-mask";
        case FormatClass::HighDynamicRange:
            return "hdr";
        case FormatClass::Count:
            break;
    }
    return "unknown";
}

FormatClass format_class_for(TextureSemantic semantic) noexcept {
    switch (semantic) {
        case TextureSemantic::Normal:
            return FormatClass::TwoChannelNormal;
        case TextureSemantic::Mask:
        case TextureSemantic::Height:
            return FormatClass::SingleChannelMask;
        case TextureSemantic::HighDynamicRange:
            return FormatClass::HighDynamicRange;
        case TextureSemantic::Colour:
        case TextureSemantic::Data:
        case TextureSemantic::UserInterface:
        case TextureSemantic::Count:
            break;
    }
    return FormatClass::BlockColour;
}

MipReduction mip_reduction_for(TextureSemantic semantic) noexcept {
    switch (semantic) {
        case TextureSemantic::Normal:
            return MipReduction::NormalRenormalised;
        case TextureSemantic::Mask:
            return MipReduction::CoveragePreserving;
        case TextureSemantic::Height:
        case TextureSemantic::Data:
            return MipReduction::Arithmetic;
        case TextureSemantic::Colour:
        case TextureSemantic::HighDynamicRange:
        case TextureSemantic::UserInterface:
        case TextureSemantic::Count:
            break;
    }
    return MipReduction::ColourSpaceAware;
}

Status validate_address(const VirtualAddress& address) noexcept {
    if (address.kind != AddressKind::Planar) {
        return make_unexpected(Error{ErrorCode::Unsupported,
                                     "virtual texturing: only the planar address kind exists; the "
                                     "encoding reserves the rest for a volumetric space"});
    }
    if (address.texture >= kMaxVirtualTextures) {
        return make_unexpected(
            Error{ErrorCode::OutOfRange, "virtual texturing: texture id exceeds 20 bits"});
    }
    if (address.mip >= kMaxMipLevels) {
        return make_unexpected(
            Error{ErrorCode::OutOfRange, "virtual texturing: mip level exceeds 5 bits"});
    }
    if (address.layer >= kMaxLayers) {
        return make_unexpected(
            Error{ErrorCode::OutOfRange, "virtual texturing: layer exceeds 3 bits"});
    }
    if (address.tile_x >= kMaxTilesPerAxis || address.tile_y >= kMaxTilesPerAxis) {
        return make_unexpected(
            Error{ErrorCode::OutOfRange, "virtual texturing: tile coordinate exceeds 12 bits"});
    }
    return ok();
}

namespace {

[[nodiscard]] u32 tiles_along(u32 extent, u8 mip, u16 tile_size) noexcept {
    if (tile_size == 0) {
        return 0;
    }
    const u32 level_extent = (extent >> mip) != 0 ? (extent >> mip) : 1U;
    return (level_extent + tile_size - 1U) / tile_size;
}

}  // namespace

u32 VirtualTextureDesc::tiles_x(u8 mip) const noexcept {
    return tiles_along(width, mip, tile_size);
}

u32 VirtualTextureDesc::tiles_y(u8 mip) const noexcept {
    return tiles_along(height, mip, tile_size);
}

u32 VirtualTextureDesc::tile_count(u8 mip) const noexcept {
    return tiles_x(mip) * tiles_y(mip);
}

u64 VirtualTextureDesc::mip_tail_bytes() const noexcept {
    u64 total = 0;
    for (u8 mip = mip_tail_base(); mip < mip_count; ++mip) {
        total += static_cast<u64>(tile_count(mip)) * bytes_per_tile * layers;
    }
    return total;
}

u8 required_border(u32 max_anisotropy) noexcept {
    const u32 aniso = (max_anisotropy < 1U) ? 1U : max_anisotropy;
    const u32 reach = ((aniso + 1U) / 2U) + 1U;
    const u32 even = (reach + 1U) & ~1U;  // whole 4x4 blocks on both edges of a tile
    return static_cast<u8>((even > 255U) ? 255U : even);
}

Status validate_description(const VirtualTextureDesc& desc) noexcept {
    if (desc.width == 0 || desc.height == 0) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "virtual texturing: a texture with no extent"});
    }
    if (desc.tile_size == 0) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "virtual texturing: a tile size of zero"});
    }
    if (desc.mip_count == 0 || desc.mip_count > kMaxMipLevels) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "virtual texturing: mip count must be between 1 and 32"});
    }
    if (desc.layers == 0 || desc.layers > kMaxLayers) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "virtual texturing: layer count must be between 1 and 8"});
    }
    if (desc.mip_tail_levels == 0 || desc.mip_tail_levels > desc.mip_count) {
        // THE TAIL IS NOT OPTIONAL. Without it a non-resident page has nothing to fall back to, and
        // the sampling path's only remaining answers are a placeholder, a black texel or a stall —
        // all three of which `virtual-texturing` forbids in a shipping build.
        return make_unexpected(
            Error{ErrorCode::InvalidArgument,
                  "virtual texturing: every virtual texture needs at least one always-resident mip "
                  "tail level, or a non-resident page has nothing to fall back to"});
    }
    if (desc.id >= kMaxVirtualTextures) {
        return make_unexpected(
            Error{ErrorCode::OutOfRange, "virtual texturing: texture id exceeds 20 bits"});
    }
    if (desc.tiles_x(0) > kMaxTilesPerAxis || desc.tiles_y(0) > kMaxTilesPerAxis) {
        return make_unexpected(Error{ErrorCode::OutOfRange,
                                     "virtual texturing: mip 0 needs more than 4096 tiles on an "
                                     "axis, which the 12-bit tile coordinate cannot address"});
    }
    if (desc.border < required_border(1)) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument,
                  "virtual texturing: the border is too small for bilinear filtering to stay "
                  "inside the tile; see required_border()"});
    }
    if (is_virtual(desc.model) && desc.bytes_per_tile == 0) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "virtual texturing: a paged texture must declare its tile "
                                     "size in bytes, or nothing can budget it"});
    }
    return ok();
}

}  // namespace cy::render::vt
