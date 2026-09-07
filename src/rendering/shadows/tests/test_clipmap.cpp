// Directional clipmaps: the snapping, and the two scenarios that follow from it. Task 8.1.
//
// "WHEN the camera moves a few centimetres THEN no clip level SHALL move and no cached page SHALL
// be invalidated by the movement" and "WHEN the camera crosses a page boundary THEN a small band of
// new pages SHALL be requested and the remainder SHALL stay cached". Both are properties of an
// integer, which is why they are asserted here rather than looked at in a debug view.

#include <cy/test/test.h>

#include <cy/rendering/shadows/clipmap.h>

namespace {

using cy::rendering::ClipmapConfig;
using cy::rendering::ClipmapLevel;
using cy::rendering::ShadowBasis;
using cy::rendering::ShadowPageGeometry;

ClipmapConfig config() noexcept {
    ClipmapConfig configuration;
    configuration.level_count = 6;
    configuration.first_level_extent = 32.0F;
    configuration.level_ratio = 2.0F;
    // 32 pages a side, so a one-page shift exposes 32 pages out of 1024 — a band, not a level.
    configuration.geometry = ShadowPageGeometry{128, 4096};
    return configuration;
}

}  // namespace

CY_TEST_CASE("a camera that moves two centimetres moves no clip level at all") {
    const ClipmapConfig configuration = config();
    const ShadowBasis basis = cy::rendering::shadow_basis(cy::Vec3{0.3F, -1.0F, 0.2F});

    const cy::Vec3 before{100.0F, 3.0F, -40.0F};
    const cy::Vec3 after{100.02F, 3.0F, -40.01F};

    for (cy::u32 level = 0; level < configuration.level_count; ++level) {
        const ClipmapLevel first = clipmap_level(configuration, basis, before, level);
        const ClipmapLevel second = clipmap_level(configuration, basis, after, level);
        CY_CHECK(first.same_footprint(second));
        CY_CHECK_EQ(first.origin_page_x, second.origin_page_x);
        CY_CHECK_EQ(first.origin_page_y, second.origin_page_y);
        CY_CHECK_EQ(clipmap_pages_entering(configuration, first, second), 0U);
    }
}

CY_TEST_CASE("crossing a page boundary exposes a band, not a level") {
    const ClipmapConfig configuration = config();
    const ShadowBasis basis = cy::rendering::shadow_basis(cy::Vec3{0.0F, -1.0F, 0.0F});
    const cy::u32 side = configuration.geometry.pages_per_side();
    const cy::u32 total = side * side;

    const ClipmapLevel start = clipmap_level(configuration, basis, cy::Vec3{0.0F, 0.0F, 0.0F}, 0);
    CY_REQUIRE_NE(start.page_world_size, 0.0F);

    // Walk along the basis' own right axis until the level shifts by exactly one page.
    ClipmapLevel shifted = start;
    cy::f32 travelled = 0.0F;
    while (shifted.origin_page_x == start.origin_page_x && travelled < start.extent) {
        travelled += start.page_world_size * 0.25F;
        shifted = clipmap_level(configuration, basis, basis.right * travelled, 0);
    }
    CY_REQUIRE_EQ(shifted.origin_page_x, start.origin_page_x + 1);
    CY_CHECK_EQ(shifted.origin_page_y, start.origin_page_y);
    CY_CHECK_EQ(clipmap_pages_entering(configuration, start, shifted), side);
    CY_CHECK_LT(clipmap_pages_entering(configuration, start, shifted), total);

    // A teleport past the level's own extent shares nothing, and the honest answer is the whole
    // level rather than a band.
    const ClipmapLevel teleported =
        clipmap_level(configuration, basis, basis.right * (start.extent * 4.0F), 0);
    CY_CHECK_EQ(clipmap_pages_entering(configuration, start, teleported), total);
}

CY_TEST_CASE("levels double their extent and halve their density, and share one lattice") {
    const ClipmapConfig configuration = config();
    const ShadowBasis basis = cy::rendering::shadow_basis(cy::Vec3{0.0F, -1.0F, 0.0F});
    const cy::Vec3 camera{17.3F, 0.0F, -4.1F};

    const ClipmapLevel level0 = clipmap_level(configuration, basis, camera, 0);
    const ClipmapLevel level1 = clipmap_level(configuration, basis, camera, 1);
    CY_CHECK_NEAR(level1.extent, level0.extent * 2.0F, 1e-4F);
    CY_CHECK_NEAR(level1.texel_world_size, level0.texel_world_size * 2.0F, 1e-6F);
    CY_CHECK_NEAR(level1.page_world_size, level0.page_world_size * 2.0F, 1e-4F);

    // Snapping is to a whole page, so a level's origin is an exact multiple of its page size. A
    // level whose origin sat on a half page would not share a lattice with the one above it, and
    // the coarser-page rung of the fallback chain would sample the wrong world square.
    const cy::f32 remainder = level0.origin_light_space.x -
                              (static_cast<cy::f32>(level0.origin_page_x) * level0.page_world_size);
    CY_CHECK_NEAR(remainder, 0.0F, 1e-3F);

    // A level is out of range: clamped rather than extrapolated.
    const ClipmapLevel clamped = clipmap_level(configuration, basis, camera, 99);
    CY_CHECK_EQ(clamped.index, configuration.level_count - 1U);
}

CY_TEST_CASE("a receiver's texel density selects the clip level, and the space it addresses") {
    const ClipmapConfig configuration = config();
    const cy::f32 base = configuration.first_level_extent /
                         static_cast<cy::f32>(configuration.geometry.virtual_texels);

    CY_CHECK_EQ(clipmap_level_for(configuration, base * 0.5F), 0U);
    CY_CHECK_EQ(clipmap_level_for(configuration, base * 2.0F), 1U);
    CY_CHECK_EQ(clipmap_level_for(configuration, base * 16.0F), 4U);
    CY_CHECK_EQ(clipmap_level_for(configuration, base * 1e6F), configuration.level_count - 1U);

    // The join: a resolved level becomes an ordinary address space, and everything downstream sees
    // that rather than a clipmap.
    const ShadowBasis basis = cy::rendering::shadow_basis(cy::Vec3{0.0F, -1.0F, 0.0F});
    const cy::Vec3 camera{5.0F, 0.0F, 5.0F};
    const ClipmapLevel level = clipmap_level(configuration, basis, camera, 2);
    const cy::rendering::ShadowAddressSpace space =
        clipmap_address_space(configuration, basis, level, 11);
    CY_CHECK_EQ(space.light_slot, 11U);
    CY_CHECK_EQ(static_cast<cy::u32>(space.level), 2U);
    const cy::rendering::ShadowAddress address = address_of(space, camera);
    CY_CHECK(address.inside);
    CY_CHECK_NEAR(shadow_texel_world_size(space, 0.0F), level.texel_world_size, 1e-6F);
}

CY_TEST_CASE("the light basis is derived from the direction alone and is stable") {
    const ShadowBasis a = cy::rendering::shadow_basis(cy::Vec3{0.0F, -2.0F, 0.0F});
    const ShadowBasis b = cy::rendering::shadow_basis(cy::Vec3{0.0F, -9.0F, 0.0F});
    // Two magnitudes of one direction produce one basis: the origin computed this frame has to be
    // comparable with the one cached last frame.
    CY_CHECK_NEAR(a.right.x, b.right.x, 1e-6F);
    CY_CHECK_NEAR(a.right.y, b.right.y, 1e-6F);
    CY_CHECK_NEAR(a.right.z, b.right.z, 1e-6F);
    CY_CHECK_NEAR(dot(a.right, a.up), 0.0F, 1e-5F);
    CY_CHECK_NEAR(dot(a.right, a.forward), 0.0F, 1e-5F);
    CY_CHECK_NEAR(length(a.up), 1.0F, 1e-5F);
}
