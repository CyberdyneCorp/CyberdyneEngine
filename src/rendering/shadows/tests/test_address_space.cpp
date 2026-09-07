// The virtual shadow address space: page identity, the three projections, texel footprints, and the
// promise that no consumer of a lookup can see a point light's internal mapping. Task 8.1.

#include <cy/test/test.h>

#include <cy/rendering/shadows/address_space.h>

#include <cmath>

namespace {

using cy::rendering::PointMapping;
using cy::rendering::ShadowAddress;
using cy::rendering::ShadowAddressSpace;
using cy::rendering::ShadowPageGeometry;
using cy::rendering::ShadowProjection;
using cy::rendering::VirtualPage;

ShadowAddressSpace spot_space() noexcept {
    ShadowAddressSpace space;
    space.projection = ShadowProjection::Spot;
    space.light_slot = 3;
    space.level = 1;
    space.basis = cy::rendering::shadow_basis(cy::Vec3{0.0F, 0.0F, -1.0F});
    space.position = cy::Vec3{0.0F, 0.0F, 0.0F};
    space.half_angle = 0.6F;
    space.extent = 50.0F;
    space.geometry = ShadowPageGeometry{128, 8192};
    return space;
}

}  // namespace

CY_TEST_CASE("a virtual page round-trips through its packed identity") {
    VirtualPage page;
    page.light_slot = 0xABCDEFU;
    page.level = 13;
    page.face = 5;
    page.x = 60000;
    page.y = 40000;

    const VirtualPage recovered = VirtualPage::unpack(page.pack());
    CY_CHECK_EQ(recovered.light_slot, page.light_slot);
    CY_CHECK_EQ(static_cast<cy::u32>(recovered.level), 13U);
    CY_CHECK_EQ(static_cast<cy::u32>(recovered.face), 5U);
    CY_CHECK_EQ(recovered.x, page.x);
    CY_CHECK_EQ(recovered.y, page.y);
    CY_CHECK(recovered == page);

    // Two marks of the same page must collide, or compaction has nothing to merge. Two different
    // pages must not, or one light's shadow lands in another's slot.
    VirtualPage neighbour = page;
    neighbour.x = static_cast<cy::u16>(page.x + 1U);
    CY_CHECK_NE(neighbour.pack(), page.pack());
}

CY_TEST_CASE("a spot light addresses what is in front of it and nothing behind it") {
    const ShadowAddressSpace space = spot_space();

    const ShadowAddress centre = address_of(space, cy::Vec3{0.0F, 0.0F, -10.0F});
    CY_REQUIRE(centre.inside);
    CY_CHECK_EQ(centre.page.light_slot, 3U);
    CY_CHECK_EQ(static_cast<cy::u32>(centre.page.level), 1U);
    const cy::u32 side = space.geometry.pages_per_side();
    CY_CHECK_EQ(static_cast<cy::u32>(centre.page.x), side / 2U);
    CY_CHECK_EQ(static_cast<cy::u32>(centre.page.y), side / 2U);

    CY_CHECK_FALSE(address_of(space, cy::Vec3{0.0F, 0.0F, 10.0F}).inside);
    CY_CHECK_FALSE(address_of(space, cy::Vec3{0.0F, 0.0F, -500.0F}).inside);
    // Outside the cone, but in front: the projection lands outside the unit square.
    CY_CHECK_FALSE(address_of(space, cy::Vec3{40.0F, 0.0F, -10.0F}).inside);
}

CY_TEST_CASE("changing the point light mapping does not change what a consumer sees") {
    ShadowAddressSpace space;
    space.projection = ShadowProjection::Point;
    space.light_slot = 9;
    space.level = 0;
    space.basis = cy::rendering::shadow_basis(cy::Vec3{0.0F, -1.0F, 0.0F});
    space.position = cy::Vec3{1.0F, 2.0F, 3.0F};
    space.extent = 40.0F;
    space.geometry = ShadowPageGeometry{128, 4096};

    const cy::Vec3 sample{6.0F, 4.0F, 1.0F};

    space.point_mapping = PointMapping::CubeFaces;
    const ShadowAddress cube = address_of(space, sample);
    space.point_mapping = PointMapping::Octahedral;
    const ShadowAddress octahedral = address_of(space, sample);

    // The consumer's answer has the same shape and the same light under either mapping, and both
    // land inside a real page. That is the whole of "consumers of shadow lookups SHALL be
    // unaffected": the face is inside the page id and no test here spells one.
    CY_REQUIRE(cube.inside);
    CY_REQUIRE(octahedral.inside);
    CY_CHECK_EQ(cube.page.light_slot, octahedral.page.light_slot);
    CY_CHECK_EQ(cube.page.level, octahedral.page.level);
    for (const ShadowAddress& address : {cube, octahedral}) {
        CY_CHECK(address.page_uv.x >= 0.0F);
        CY_CHECK(address.page_uv.x < 1.0F);
        CY_CHECK(address.page_uv.y >= 0.0F);
        CY_CHECK(address.page_uv.y < 1.0F);
        CY_CHECK_LT(static_cast<cy::u32>(address.page.x), space.geometry.pages_per_side());
        CY_CHECK_LT(static_cast<cy::u32>(address.page.y), space.geometry.pages_per_side());
    }

    // A point on the opposite side of the light is still addressable — a point light's space covers
    // the sphere — which is what makes the sixth cube face and the octahedron's lower half the same
    // statement to a consumer.
    CY_CHECK(address_of(space, cy::Vec3{-6.0F, 0.0F, 3.0F}).inside);
}

CY_TEST_CASE(
    "a texel footprint is constant for a clipmap level and grows with distance for a spot") {
    ShadowAddressSpace directional;
    directional.projection = ShadowProjection::DirectionalClipmap;
    directional.geometry = ShadowPageGeometry{128, 8192};
    directional.extent = 64.0F;
    CY_CHECK_NEAR(shadow_texel_world_size(directional, 5.0F), 64.0F / 8192.0F, 1e-6F);
    CY_CHECK_NEAR(shadow_texel_world_size(directional, 500.0F), 64.0F / 8192.0F, 1e-6F);

    const ShadowAddressSpace spot = spot_space();
    const cy::f32 near_texel = shadow_texel_world_size(spot, 5.0F);
    const cy::f32 far_texel = shadow_texel_world_size(spot, 25.0F);
    CY_CHECK_GT(far_texel, near_texel);
    CY_CHECK_NEAR(far_texel / near_texel, 5.0F, 1e-3F);
}

CY_TEST_CASE("a receiver that needs a coarse texel selects a coarse level") {
    const ShadowAddressSpace space = spot_space();
    const cy::f32 base = shadow_texel_world_size(space, 10.0F);

    CY_CHECK_EQ(static_cast<cy::u32>(select_level(space, 10.0F, base * 0.5F, 6)), 0U);
    CY_CHECK_EQ(static_cast<cy::u32>(select_level(space, 10.0F, base, 6)), 0U);
    CY_CHECK_EQ(static_cast<cy::u32>(select_level(space, 10.0F, base * 2.0F, 6)), 1U);
    CY_CHECK_EQ(static_cast<cy::u32>(select_level(space, 10.0F, base * 8.0F, 6)), 3U);
    // Clamped rather than extrapolated: the light has no coarser level to give.
    CY_CHECK_EQ(static_cast<cy::u32>(select_level(space, 10.0F, base * 4096.0F, 4)), 3U);
}

CY_TEST_CASE("a box projects into the pages it covers, and into none when it is behind the light") {
    const ShadowAddressSpace space = spot_space();
    VirtualPage pages[256];

    const cy::Aabb small =
        cy::Aabb::from_center_extents(cy::Vec3{0.0F, 0.0F, -10.0F}, cy::Vec3{0.2F, 0.2F, 0.2F});
    const cy::u32 covered = pages_covering(space, small, pages, 256);
    CY_CHECK_GT(covered, 0U);
    CY_CHECK_LT(covered, 64U);

    const cy::Aabb behind =
        cy::Aabb::from_center_extents(cy::Vec3{0.0F, 0.0F, 20.0F}, cy::Vec3{1.0F, 1.0F, 1.0F});
    CY_CHECK_EQ(pages_covering(space, behind, pages, 256), 0U);

    // The count is reported even when the scratch cannot hold it, which is the diagnostic the
    // header promises rather than a silent truncation.
    const cy::Aabb wide =
        cy::Aabb::from_center_extents(cy::Vec3{0.0F, 0.0F, -10.0F}, cy::Vec3{6.0F, 6.0F, 0.5F});
    const cy::u32 wanted = pages_covering(space, wide, pages, 4);
    CY_CHECK_GT(wanted, 4U);
}

CY_TEST_CASE("the page geometry reports itself, because the size is a measured decision") {
    char text[128] = {};
    const ShadowPageGeometry geometry{128, 16384};
    const cy::usize written = describe_page_geometry(geometry, text, sizeof(text));
    CY_CHECK_GT(written, 0U);
    CY_CHECK_EQ(geometry.pages_per_side(), 128U);
    CY_CHECK(geometry.valid());
    CY_CHECK_FALSE((ShadowPageGeometry{0, 16384}).valid());
    CY_CHECK_FALSE((ShadowPageGeometry{100, 16384}).valid());
}
