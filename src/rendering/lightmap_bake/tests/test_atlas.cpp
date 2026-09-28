// SPDX-License-Identifier: MIT
// The shared atlas and the address word. `unit.render_lightmap_atlas`.
//
// `rendering-global-illumination` — "UV2 and chart packing": charts packed into atlases with
// padding for bilinear filtering and the mip chain at the bake resolution; "Lightmap baking": per
// object resolution scaling over a global texel density.

#include <cy/rendering/lightmap_bake/atlas.h>
#include <cy/test/test.h>

#include <cmath>
#include <vector>

namespace {

using namespace cy::rendering::lightmap_bake;  // NOLINT(google-build-using-namespace)
using cy::f32;
using cy::u32;
using cy::usize;
using cy::Vec2;

[[nodiscard]] AtlasLayout pack(const std::vector<AtlasObject>& objects,
                               const AtlasSettings& settings) {
    AtlasLayout layout;
    CY_REQUIRE(pack_atlas({objects.data(), objects.size()}, settings, layout).has_value());
    return layout;
}

[[nodiscard]] bool overlaps(const AtlasPlacement& a, const AtlasPlacement& b) {
    return a.page == b.page && a.block_x < b.block_x + b.block_width &&
           b.block_x < a.block_x + a.block_width && a.block_y < b.block_y + b.block_height &&
           b.block_y < a.block_y + a.block_height;
}

}  // namespace

CY_TEST_CASE("every object of a level lands in shared pages, and no two rectangles overlap") {
    std::vector<AtlasObject> objects;
    for (u32 index = 0; index < 40; ++index) {
        AtlasObject object;
        object.surface_area = 2.0F + static_cast<f32>(index % 7U) * 3.0F;
        object.aspect = index % 3U == 0U ? 2.0F : 1.0F;
        objects.push_back(object);
    }
    AtlasSettings settings;
    settings.page_size = 256;
    // Dense enough that the level fills most of a page: occupancy is measured over every page
    // opened, so a level that uses a fifth of its only page reports a fifth whatever the packer
    // did.
    settings.texel_density = 8.0F;
    const AtlasLayout layout = pack(objects, settings);

    // Shared: forty objects in far fewer pages than forty, and tightly.
    CY_CHECK_GE(layout.pages, 1U);
    CY_CHECK_LT(layout.pages, 4U);
    CY_CHECK_GT(layout.occupancy, 0.5F);
    for (usize a = 0; a < layout.placements.size(); ++a) {
        const AtlasPlacement& placement = layout.placements[a];
        CY_CHECK_LE(placement.block_x + placement.block_width, kAddressBlocks);
        CY_CHECK_LE(placement.block_y + placement.block_height, kAddressBlocks);
        for (usize b = a + 1; b < layout.placements.size(); ++b) {
            CY_CHECK_FALSE(overlaps(placement, layout.placements[b]));
        }
    }
}

CY_TEST_CASE("an object's resolution scale multiplies the level's texel density") {
    AtlasObject plain;
    plain.surface_area = 16.0F;
    AtlasObject doubled = plain;
    doubled.resolution_scale = 2.0F;
    AtlasSettings settings;
    settings.page_size = 1024;
    settings.texel_density = 4.0F;
    const AtlasLayout layout = pack({plain, doubled}, settings);
    const u32 block = layout.block_texels;
    const auto interior = [&](const AtlasPlacement& placement) {
        return (placement.block_width * block) - (2U * layout.gutter_texels);
    };
    // 16 m² at 4 texels/m is 16x16 texels of interior at least, and twice the scale is twice the
    // side: the rectangle is rounded UP to the block grid, never down.
    CY_CHECK_GE(interior(layout.placements[0]), 16U);
    CY_CHECK_GE(interior(layout.placements[1]), 32U);
    CY_CHECK_GT(layout.placements[1].block_width, layout.placements[0].block_width);

    settings.texel_density = 8.0F;
    const AtlasLayout denser = pack({plain}, settings);
    CY_CHECK_GE(interior(denser.placements[0]), 32U);
}

CY_TEST_CASE("rectangles sit on the block grid with a gutter that survives the declared mips") {
    AtlasSettings settings;
    settings.page_size = 512;  // 4-texel blocks
    settings.mip_levels = 2;
    CY_CHECK_EQ(gutter_for(settings), 4U);
    // A mip level coarser than a block would move a rectangle boundary off a texel boundary, so
    // the padding is not declared for it: three levels on 4-texel blocks is two.
    settings.mip_levels = 3;
    CY_CHECK_EQ(gutter_for(settings), 4U);
    // And bilinear alone still wants two texels.
    settings.mip_levels = 0;
    CY_CHECK_EQ(gutter_for(settings), 2U);

    settings.mip_levels = 2;
    AtlasObject object;
    object.surface_area = 5.0F;
    const AtlasLayout layout = pack({object, object, object}, settings);
    CY_CHECK_EQ(layout.block_texels, 4U);
    CY_CHECK_EQ(layout.mip_levels, 2U);
    // Two rectangles side by side on a shelf: interiors are two gutters apart at least, which at
    // mip level 2 is still a whole texel of each object's own gutter between them.
    const AtlasPlacement& first = layout.placements[0];
    const AtlasPlacement& second = layout.placements[1];
    CY_REQUIRE_EQ(first.block_y, second.block_y);
    const f32 first_end = atlas_coordinate(encode_address(first), Vec2{1.0F, 0.5F}, 512, 4).x;
    const f32 second_start = atlas_coordinate(encode_address(second), Vec2{0.0F, 0.5F}, 512, 4).x;
    CY_CHECK_GE(std::abs(second_start - first_end), 8.0F);
}

CY_TEST_CASE("the address word round-trips, and zero is no lightmap") {
    AtlasPlacement placement;
    placement.page = 3;
    placement.block_x = 17;
    placement.block_y = 99;
    placement.block_width = 11;
    placement.block_height = 128 - 99;
    const u32 address = encode_address(placement);
    CY_CHECK_NE(address, kNoLightmapAddress);
    AtlasPlacement decoded;
    CY_REQUIRE(decode_address(address, decoded));
    CY_CHECK_EQ(decoded.page, 3U);
    CY_CHECK_EQ(decoded.block_x, 17U);
    CY_CHECK_EQ(decoded.block_y, 99U);
    CY_CHECK_EQ(decoded.block_width, 11U);
    CY_CHECK_EQ(decoded.block_height, 29U);
    // The first rectangle of the first page is a real address, not the sentinel: the page is
    // stored plus one for exactly this reason.
    AtlasPlacement origin;
    origin.block_width = 1;
    origin.block_height = 1;
    CY_CHECK_NE(encode_address(origin), kNoLightmapAddress);
    CY_CHECK_FALSE(decode_address(kNoLightmapAddress, decoded));

    // The interior's corners: UV2 (0, 0) is one gutter in from the rectangle's corner, and the
    // page is stacked below the others.
    const Vec2 corner = atlas_coordinate(address, Vec2{0.0F, 0.0F}, 256, 2);
    CY_CHECK_NEAR(corner.x, (17.0F * 2.0F) + 2.0F, 1.0e-4F);
    CY_CHECK_NEAR(corner.y, (3.0F * 256.0F) + (99.0F * 2.0F) + 2.0F, 1.0e-4F);
    const Vec2 far = atlas_coordinate(address, Vec2{1.0F, 1.0F}, 256, 2);
    CY_CHECK_NEAR(far.x - corner.x, (11.0F * 2.0F) - 4.0F, 1.0e-4F);
}

CY_TEST_CASE("a level that does not fit is refused, and a bad page size is refused") {
    AtlasObject huge;
    huge.surface_area = 1.0e6F;
    AtlasSettings settings;
    settings.page_size = 128;
    settings.max_pages = 2;
    std::vector<AtlasObject> many(5, huge);
    AtlasLayout layout;
    // Each asks for more than a page and is clamped to one; five do not fit in two.
    CY_CHECK_FALSE(pack_atlas({many.data(), many.size()}, settings, layout).has_value());
    settings.page_size = 300;
    CY_CHECK_FALSE(pack_atlas({many.data(), 1}, settings, layout).has_value());
}
