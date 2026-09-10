// The navigation mesh: tiles published and released, adjacency within and across a tile boundary,
// stale references, obstacles and off-mesh links. M8.b task 6.1.
//
// THE CASE THIS SUITE EXISTS FOR is "a released tile's references stop resolving". `navigation`
// requires that a region unloading under an agent invalidates its path "rather than dereferencing
// released data", and a salt that did not advance would make that failure silent and occasional.

#include <cy/core/memory/system_allocator.h>
#include <cy/navigation/navmesh.h>
#include <cy/test/test.h>

#include "nav_fixture.h"

using namespace cy;
using namespace cy::navigation;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

}  // namespace

CY_TEST_CASE("a published tile's polygons are adjacent to each other") {
    NavMesh mesh = testing::single_tile_mesh(allocator());

    const NavMesh::Stats stats = mesh.stats();
    CY_CHECK_EQ(stats.tiles, 1U);
    CY_CHECK_EQ(stats.polys, 16U);
    // A 4x4 grid of quads has 2*4*3 = 24 internal edges and 16 on the border.
    CY_CHECK_EQ(stats.border_edges, 16U);

    const u32 slot = mesh.tile_slot(TileCoord{0, 0, 0});
    CY_REQUIRE_NE(slot, 0xFFFFFFFFU);
    // The centre quad of the grid has four neighbours; a corner quad has two.
    u32 interior_neighbours = 0;
    for (const PolyRef neighbour : mesh.poly_neighbours(mesh.tile_poly(slot, 5))) {
        if (neighbour.valid()) {
            ++interior_neighbours;
        }
    }
    CY_CHECK_EQ(interior_neighbours, 4U);
}

CY_TEST_CASE("two tiles connect across their shared border") {
    NavMesh mesh(allocator(), Name::intern("test.nav"), 8.0F);
    const Expected<TileChange, Error> first =
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{0, 0, 0}, 8.0F, 4));
    CY_REQUIRE(first.has_value());
    // `TileChange::border_edges` counts the edges of THIS publication that found a partner in a
    // neighbouring tile — not the tile's unmatched boundary. The first tile has no neighbour, so it
    // makes none. (`NavMesh::Stats::border_edges` is the other quantity and counts the unmatched
    // ones; the case above asserts that it is 16 for exactly this tile.)
    CY_CHECK_EQ(first->border_edges, 0U);

    const Expected<TileChange, Error> second =
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{1, 0, 0}, 8.0F, 4));
    CY_REQUIRE(second.has_value());
    // The second tile's western border edges found partners in the first, so four of its sixteen
    // border edges became adjacency instead.
    CY_CHECK_EQ(second->internal_edges, 24U);
    CY_CHECK_EQ(second->border_edges, 4U);

    const u32 slot = mesh.tile_slot(TileCoord{0, 0, 0});
    const u32 other = mesh.tile_slot(TileCoord{1, 0, 0});
    bool crosses = false;
    for (u32 index = 0; index < mesh.tile_poly_count(slot); ++index) {
        for (const PolyRef neighbour : mesh.poly_neighbours(mesh.tile_poly(slot, index))) {
            crosses = crosses || (neighbour.valid() && neighbour.tile() == other);
        }
    }
    CY_CHECK(crosses);
}

CY_TEST_CASE(
    "a released tile's references stop resolving rather than resolving to something else") {
    NavMesh mesh = testing::single_tile_mesh(allocator());
    const u32 slot = mesh.tile_slot(TileCoord{0, 0, 0});
    const PolyRef held = mesh.tile_poly(slot, 7);
    CY_REQUIRE(mesh.poly(held) != nullptr);
    const u32 version_before = mesh.version();

    const Status removed = mesh.remove_tile(TileCoord{0, 0, 0});
    CY_REQUIRE(removed.has_value());
    CY_CHECK_EQ(mesh.tile_count(), 0U);
    CY_CHECK_GT(mesh.version(), version_before);
    CY_CHECK(mesh.poly(held) == nullptr);

    // The slot is reused by the next publication, and the reference STILL does not resolve: that is
    // what the salt is for, and without it `held` would now name a different polygon.
    const Expected<TileChange, Error> republished =
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{0, 0, 0}, 8.0F, 4));
    CY_REQUIRE(republished.has_value());
    CY_CHECK_EQ(republished->tile, slot);
    CY_CHECK(mesh.poly(held) == nullptr);
}

CY_TEST_CASE("republishing one tile leaves its neighbour's polygons resolvable") {
    NavMesh mesh(allocator(), Name::intern("test.nav"), 8.0F);
    const Expected<TileChange, Error> first =
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{0, 0, 0}, 8.0F, 4));
    CY_REQUIRE(first.has_value());
    const Expected<TileChange, Error> second =
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{1, 0, 0}, 8.0F, 4));
    CY_REQUIRE(second.has_value());

    const PolyRef in_neighbour = mesh.tile_poly(mesh.tile_slot(TileCoord{1, 0, 0}), 0);
    const Expected<TileChange, Error> rebuilt =
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{0, 0, 0}, 8.0F, 4));
    CY_REQUIRE(rebuilt.has_value());
    CY_CHECK(rebuilt->replaced_existing);
    // Only the rebuilt tile's own references went stale. `navigation`'s "only that tile SHALL be
    // rebuilt, with neighbouring tiles' adjacency updated".
    CY_CHECK(mesh.poly(in_neighbour) != nullptr);
    CY_CHECK_EQ(rebuilt->border_edges, 4U);
}

CY_TEST_CASE("find_nearest answers nothing when the mesh is outside the extents it was given") {
    // REGRESSION. `find_nearest` used its `extents` only to reject whole TILES, and then took the
    // closest polygon in a surviving tile however far away it was — so a query in the middle of a
    // hole reported ground, `add_link` snapped a jump's endpoint across it rather than refusing,
    // and a path started somewhere the agent was not. The fix bounds the candidate itself.
    NavMesh mesh = testing::single_tile_mesh(allocator());
    Vec3 nearest;

    // Inside the tile's bounds, four metres above the surface, with a half-metre vertical extent.
    CY_CHECK_FALSE(
        mesh.find_nearest(Vec3{4.0F, 4.0F, 4.0F}, Vec3{0.5F, 0.5F, 0.5F}, kAllAreas, nearest)
            .valid());
    // The same point with an extent that reaches the surface does resolve.
    CY_CHECK(mesh.find_nearest(Vec3{4.0F, 4.0F, 4.0F}, Vec3{0.5F, 5.0F, 0.5F}, kAllAreas, nearest)
                 .valid());
    // Inside the tile's bounds in x and z, well outside them in neither: on the surface, it does.
    CY_CHECK(mesh.find_nearest(Vec3{4.0F, 0.0F, 4.0F}, Vec3{0.5F, 0.5F, 0.5F}, kAllAreas, nearest)
                 .valid());
}

CY_TEST_CASE("an obstacle marks a footprint without a rebuild") {
    NavMesh mesh = testing::single_tile_mesh(allocator());
    const u32 builds_before = mesh.stats().tile_builds;

    Vec3 nearest;
    const PolyRef under =
        mesh.find_nearest(Vec3{3.0F, 0.0F, 3.0F}, Vec3{2.0F, 2.0F, 2.0F}, kAllAreas, nearest);
    CY_REQUIRE(under.valid());
    CY_CHECK_EQ(mesh.effective_area(under), kAreaGround);

    NavObstacleShape crate;
    crate.centre = nearest;
    crate.radius = 1.0F;
    crate.height = 2.0F;
    crate.area = kAreaNull;
    const Expected<ObstacleId, Error> id = mesh.add_obstacle(crate);
    CY_REQUIRE(id.has_value());

    CY_CHECK_EQ(mesh.effective_area(under), kAreaNull);
    // Nothing was voxelised: `navigation`'s "rather than triggering a voxelisation rebuild".
    CY_CHECK_EQ(mesh.stats().tile_builds, builds_before);

    const Status removed = mesh.remove_obstacle(*id);
    CY_REQUIRE(removed.has_value());
    CY_CHECK_EQ(mesh.effective_area(under), kAreaGround);
}

CY_TEST_CASE("an off-mesh link whose ends miss the mesh is refused") {
    NavMesh mesh = testing::single_tile_mesh(allocator());

    NavLink good;
    good.from = Vec3{1.0F, 0.0F, 1.0F};
    good.to = Vec3{7.0F, 0.0F, 7.0F};
    good.action = Name::intern("Jump");
    good.requires_capabilities = CapabilityMask{1} << 3U;
    const Expected<LinkId, Error> id = mesh.add_link(good, Vec3{1.0F, 1.0F, 1.0F});
    CY_REQUIRE(id.has_value());
    CY_CHECK_EQ(mesh.link_count(), 1U);
    CY_REQUIRE(mesh.link(*id) != nullptr);
    CY_CHECK(mesh.link(*id)->from_poly.valid());

    // Both directions of a bidirectional link are reachable from both ends.
    CY_CHECK_EQ(mesh.links_from(mesh.link(*id)->from_poly).size(), usize{1});
    CY_CHECK_EQ(mesh.links_from(mesh.link(*id)->to_poly).size(), usize{1});

    NavLink dangling;
    dangling.from = Vec3{1.0F, 0.0F, 1.0F};
    dangling.to = Vec3{500.0F, 0.0F, 500.0F};
    const Expected<LinkId, Error> refused = mesh.add_link(dangling, Vec3{1.0F, 1.0F, 1.0F});
    CY_CHECK_FALSE(refused.has_value());
}

CY_TEST_CASE("a link over a rebuilt tile is re-snapped rather than left dangling") {
    NavMesh mesh = testing::single_tile_mesh(allocator());
    NavLink ladder;
    ladder.from = Vec3{1.0F, 0.0F, 1.0F};
    ladder.to = Vec3{7.0F, 0.0F, 7.0F};
    ladder.action = Name::intern("Ladder");
    const Expected<LinkId, Error> id = mesh.add_link(ladder, Vec3{1.0F, 1.0F, 1.0F});
    CY_REQUIRE(id.has_value());

    const Expected<TileChange, Error> rebuilt =
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{0, 0, 0}, 8.0F, 4));
    CY_REQUIRE(rebuilt.has_value());

    const NavLink* live = mesh.link(*id);
    CY_REQUIRE(live != nullptr);
    CY_CHECK(live->from_poly.valid());
    CY_CHECK(mesh.poly(live->from_poly) != nullptr);
    CY_CHECK(mesh.poly(live->to_poly) != nullptr);
}
