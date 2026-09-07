// The virtual address space and what an asset declares. M6 task 5.1.
//
// `virtual-texturing` — "Virtual address space". Two clauses are checkable here and both are cases:
// "Physical layout is private" — nothing in `VirtualTextureDesc` can hold a physical coordinate,
// which is a property of the type rather than of a review — and the versioned encoding that lets a
// volumetric address space arrive later "without invalidating existing content".

#include <cy/servers/render/virtual_texturing/address.h>
#include <cy/test/test.h>

using namespace cy::render::vt;
using cy::u32;
using cy::u64;

namespace {

[[nodiscard]] VirtualTextureDesc terrain() noexcept {
    VirtualTextureDesc desc;
    desc.id = 17;
    desc.width = 16384;
    desc.height = 16384;
    desc.tile_size = 128;
    desc.border = 4;
    desc.mip_count = 8;
    desc.layers = 1;
    desc.mip_tail_levels = 2;
    desc.semantic = TextureSemantic::Colour;
    desc.model = ResidencyModel::VirtualStreamed;
    desc.bytes_per_tile = 16384;
    return desc;
}

}  // namespace

CY_TEST_CASE("an address round-trips through its own encoding") {
    VirtualAddress address;
    address.texture = 0xFFFFF;  // the full 20 bits
    address.mip = 31;
    address.layer = 7;
    address.tile_x = 4095;
    address.tile_y = 4095;

    const u64 encoded = address.encode();
    CY_CHECK(VirtualAddress::decode(encoded) == address);
    // And it fits in the 56 bits `cy::residency::PageKey` reserves, which is what lets the
    // residency layer score a texture tile without knowing what one is.
    CY_CHECK_LT(encoded, static_cast<u64>(1) << 56U);
}

CY_TEST_CASE("the encoding is versioned, so a volumetric space can arrive later") {
    VirtualAddress planar;
    planar.texture = 3;
    planar.mip = 2;
    CY_CHECK(planar.kind == AddressKind::Planar);
    CY_CHECK_EQ(planar.encode() >> 52U, 0U);

    // A future kind occupies the top four bits and every planar address still decodes to itself.
    const u64 future = planar.encode() | (static_cast<u64>(5) << 52U);
    CY_CHECK_EQ(static_cast<u32>(VirtualAddress::decode(future).kind), 5U);
    CY_CHECK_EQ(VirtualAddress::decode(planar.encode()).mip, 2U);
    CY_CHECK_FALSE(validate_address(VirtualAddress::decode(future)));
}

CY_TEST_CASE("a field beyond its bit budget is reported rather than silently wrapped") {
    VirtualAddress address;
    CY_CHECK(validate_address(address));
    address.texture = kMaxVirtualTextures;
    CY_CHECK_FALSE(validate_address(address));
    address = VirtualAddress{};
    address.mip = 32;
    CY_CHECK_FALSE(validate_address(address));
    address = VirtualAddress{};
    address.layer = 8;
    CY_CHECK_FALSE(validate_address(address));
    address = VirtualAddress{};
    address.tile_x = 4096;
    CY_CHECK_FALSE(validate_address(address));
}

CY_TEST_CASE("the semantic decides the cache class and the mip reduction, in one place") {
    CY_CHECK(format_class_for(TextureSemantic::Normal) == FormatClass::TwoChannelNormal);
    CY_CHECK(format_class_for(TextureSemantic::Mask) == FormatClass::SingleChannelMask);
    CY_CHECK(format_class_for(TextureSemantic::HighDynamicRange) == FormatClass::HighDynamicRange);
    CY_CHECK(format_class_for(TextureSemantic::Colour) == FormatClass::BlockColour);

    // `virtual-texturing`: "Normals stay normalised" and "Distant foliage does not thin".
    CY_CHECK(mip_reduction_for(TextureSemantic::Normal) == MipReduction::NormalRenormalised);
    CY_CHECK(mip_reduction_for(TextureSemantic::Mask) == MipReduction::CoveragePreserving);
    CY_CHECK(mip_reduction_for(TextureSemantic::Height) == MipReduction::Arithmetic);
    CY_CHECK(mip_reduction_for(TextureSemantic::Colour) == MipReduction::ColourSpaceAware);
}

CY_TEST_CASE("StreamedMip is a first-class model and is not virtual") {
    // "virtualising every texture would be a regression for most of a project's content".
    CY_CHECK_FALSE(is_virtual(ResidencyModel::Resident));
    CY_CHECK_FALSE(is_virtual(ResidencyModel::StreamedMip));
    CY_CHECK(is_virtual(ResidencyModel::VirtualStreamed));
    CY_CHECK(is_virtual(ResidencyModel::VirtualRuntime));
}

CY_TEST_CASE("the tile pyramid rounds up, so a partial edge is a tile rather than a hole") {
    VirtualTextureDesc desc = terrain();
    CY_CHECK_EQ(desc.tiles_x(0), 128U);  // 16384 / 128
    CY_CHECK_EQ(desc.tile_count(0), 128U * 128U);
    CY_CHECK_EQ(desc.tiles_x(7), 1U);

    desc.width = 16385;  // one texel over a whole number of tiles
    CY_CHECK_EQ(desc.tiles_x(0), 129U);
}

CY_TEST_CASE("the mip tail is the coarsest levels, and it is not optional") {
    const VirtualTextureDesc desc = terrain();
    CY_CHECK_EQ(desc.coarsest_mip(), 7U);
    CY_CHECK_EQ(desc.mip_tail_base(), 6U);
    CY_CHECK(desc.in_mip_tail(6));
    CY_CHECK(desc.in_mip_tail(7));
    CY_CHECK_FALSE(desc.in_mip_tail(5));
    CY_CHECK_GT(desc.mip_tail_bytes(), 0U);

    VirtualTextureDesc no_tail = desc;
    no_tail.mip_tail_levels = 0;
    // Without a tail a non-resident page has nothing to fall back to, and the sampler's only
    // remaining answers are a placeholder, a black texel or a stall — all three forbidden.
    CY_CHECK_FALSE(validate_description(no_tail));
}

CY_TEST_CASE("a border is sized for the filtering the sampling path may use") {
    CY_CHECK_EQ(required_border(1), 2U);  // bilinear alone
    CY_CHECK_EQ(required_border(8), 6U);
    CY_CHECK_EQ(required_border(16), 10U);
    // Always even, so a block-compressed tile's border is a whole number of 4x4 blocks per edge.
    for (u32 aniso = 1; aniso <= 16; ++aniso) {
        CY_CHECK_EQ(required_border(aniso) % 2U, 0U);
    }

    VirtualTextureDesc thin = terrain();
    thin.border = 1;
    CY_CHECK_FALSE(validate_description(thin));
}

CY_TEST_CASE("a description that cannot be paged is refused at configuration time") {
    CY_CHECK(validate_description(terrain()));

    VirtualTextureDesc broken = terrain();
    broken.width = 0;
    CY_CHECK_FALSE(validate_description(broken));

    broken = terrain();
    broken.mip_count = 0;
    CY_CHECK_FALSE(validate_description(broken));

    broken = terrain();
    broken.layers = 9;
    CY_CHECK_FALSE(validate_description(broken));

    broken = terrain();
    broken.bytes_per_tile = 0;
    // A paged texture that cannot state its tile size is one no budget can apportion.
    CY_CHECK_FALSE(validate_description(broken));

    broken = terrain();
    broken.width = (4096 * 128) + 1;  // more than 4096 tiles on an axis
    broken.height = 128;
    CY_CHECK_FALSE(validate_description(broken));
}
