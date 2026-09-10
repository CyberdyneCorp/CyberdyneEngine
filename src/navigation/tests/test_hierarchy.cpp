// The region hierarchy: regions built per tile, planned over abstractly, refined locally, and
// invalidated regionally. M8.b task 6.1.

#include <cy/core/memory/system_allocator.h>
#include <cy/navigation/hierarchy.h>
#include <cy/test/test.h>

#include "nav_fixture.h"

using namespace cy;
using namespace cy::navigation;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// A row of `count` tiles from x = 0 east, published and registered in `hierarchy`.
void publish_row(NavMesh& mesh, NavHierarchy& hierarchy, u32 count) noexcept {
    for (u32 index = 0; index < count; ++index) {
        const TileCoord coord{static_cast<i32>(index), 0, 0};
        CY_REQUIRE(mesh.add_tile(testing::grid_tile(allocator(), coord, 8.0F, 4)).has_value());
    }
    // Registered after every tile is published, so cross-tile adjacency is already in the mesh.
    for (u32 index = 0; index < count; ++index) {
        CY_REQUIRE(
            hierarchy.rebuild_tile(mesh, TileCoord{static_cast<i32>(index), 0, 0}).has_value());
    }
}

}  // namespace

CY_TEST_CASE("one connected tile is one region, and a disconnected pair is two") {
    NavMesh mesh(allocator(), Name::intern("test.nav"), 8.0F);
    NavHierarchy hierarchy(allocator());
    publish_row(mesh, hierarchy, 1);

    CY_CHECK_EQ(hierarchy.live_region_count(), 1U);
    const RegionId region =
        hierarchy.region_at(mesh, Vec3{4.0F, 0.0F, 4.0F}, Vec3{2.0F, 2.0F, 2.0F});
    CY_REQUIRE_NE(region, kInvalidRegion);
    CY_REQUIRE(hierarchy.region(region) != nullptr);
    CY_CHECK_EQ(hierarchy.region(region)->polys, 16U);
    CY_CHECK(hierarchy.region(region)->resident);

    // A tile two coordinates away shares no edge, so it is its own region with no abstract edge.
    CY_REQUIRE(
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{2, 0, 0}, 8.0F, 4)).has_value());
    CY_REQUIRE(hierarchy.rebuild_tile(mesh, TileCoord{2, 0, 0}).has_value());
    CY_CHECK_EQ(hierarchy.live_region_count(), 2U);
    CY_CHECK(hierarchy.edges_of(region).empty());
}

CY_TEST_CASE("a cross-map plan runs over the region graph rather than the polygon graph") {
    NavMesh mesh(allocator(), Name::intern("test.nav"), 8.0F);
    NavHierarchy hierarchy(allocator());
    publish_row(mesh, hierarchy, 8);

    CY_CHECK_EQ(hierarchy.live_region_count(), 8U);
    const RegionId from = hierarchy.region_at(mesh, Vec3{1.0F, 0.0F, 3.0F}, Vec3{2.0F, 2.0F, 2.0F});
    const RegionId to = hierarchy.region_at(mesh, Vec3{61.0F, 0.0F, 3.0F}, Vec3{2.0F, 2.0F, 2.0F});
    CY_REQUIRE_NE(from, kInvalidRegion);
    CY_REQUIRE_NE(to, kInvalidRegion);

    const Expected<AbstractPath, Error> plan = hierarchy.plan(from, to);
    CY_REQUIRE(plan.has_value());
    CY_CHECK(plan->found);
    CY_CHECK(plan->complete);
    // Eight tiles in a row: the abstract path is eight nodes, and the polygon graph it stands over
    // is a hundred and twenty-eight.
    CY_CHECK_EQ(plan->regions.size(), usize{8});
    CY_CHECK_GT(plan->cost, 0.0F);
}

CY_TEST_CASE("a plan is deterministic and its segments refine one at a time") {
    NavMesh mesh(allocator(), Name::intern("test.nav"), 8.0F);
    NavHierarchy hierarchy(allocator());
    publish_row(mesh, hierarchy, 4);

    const RegionId from = hierarchy.region_at(mesh, Vec3{1.0F, 0.0F, 3.0F}, Vec3{2.0F, 2.0F, 2.0F});
    const RegionId to = hierarchy.region_at(mesh, Vec3{29.0F, 0.0F, 3.0F}, Vec3{2.0F, 2.0F, 2.0F});
    const Expected<AbstractPath, Error> first = hierarchy.plan(from, to);
    const Expected<AbstractPath, Error> second = hierarchy.plan(from, to);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_REQUIRE_EQ(first->regions.size(), second->regions.size());
    for (usize index = 0; index < first->regions.size(); ++index) {
        CY_REQUIRE_EQ(first->regions[index], second->regions[index]);
    }

    // `navigation`: "subsequent segments SHALL be refined as needed, spreading cost over time".
    PathCorridor corridor(allocator());
    u32 refined = 0;
    Vec3 cursor{1.0F, 0.0F, 3.0F};
    for (u32 segment = 0; segment < first->regions.size(); ++segment) {
        corridor.clear();
        const Expected<PathResult, Error> leg = hierarchy.refine(
            mesh, *first, segment, cursor, Vec3{29.0F, 0.0F, 3.0F}, PathFilter{}, corridor);
        CY_REQUIRE(leg.has_value());
        CY_CHECK(leg->found);
        ++refined;
        const NavRegion* next = (segment + 1 < first->regions.size())
                                    ? hierarchy.region(first->regions[segment + 1])
                                    : nullptr;
        cursor = (next != nullptr) ? next->centre : cursor;
    }
    CY_CHECK_EQ(refined, static_cast<u32>(first->regions.size()));
}

CY_TEST_CASE("a released region keeps its identity, so a plan crosses it and refinement waits") {
    // `navigation`: "Paths crossing into unloaded regions SHALL be resolvable at the abstract
    // level, with local refinement deferred until the region is resident."
    NavMesh mesh(allocator(), Name::intern("test.nav"), 8.0F);
    NavHierarchy hierarchy(allocator());
    publish_row(mesh, hierarchy, 4);

    const RegionId from = hierarchy.region_at(mesh, Vec3{1.0F, 0.0F, 3.0F}, Vec3{2.0F, 2.0F, 2.0F});
    const RegionId to = hierarchy.region_at(mesh, Vec3{29.0F, 0.0F, 3.0F}, Vec3{2.0F, 2.0F, 2.0F});
    CY_REQUIRE_NE(from, kInvalidRegion);
    CY_REQUIRE_NE(to, kInvalidRegion);

    CY_REQUIRE(hierarchy.release_tile(TileCoord{2, 0, 0}).has_value());
    CY_REQUIRE(mesh.remove_tile(TileCoord{2, 0, 0}).has_value());

    const Expected<AbstractPath, Error> plan = hierarchy.plan(from, to);
    CY_REQUIRE(plan.has_value());
    CY_CHECK(plan->found);
    CY_CHECK_FALSE(plan->complete);
    CY_CHECK_EQ(plan->first_unresident, 2U);
    CY_CHECK_EQ(hierarchy.live_region_count(), 4U);

    // The first segment refines now; the one into the released region does not, and says why.
    PathCorridor corridor(allocator());
    const Expected<PathResult, Error> near = hierarchy.refine(
        mesh, *plan, 0, Vec3{1.0F, 0.0F, 3.0F}, Vec3{29.0F, 0.0F, 3.0F}, PathFilter{}, corridor);
    CY_CHECK(near.has_value());
    corridor.clear();
    const Expected<PathResult, Error> deferred = hierarchy.refine(
        mesh, *plan, 2, Vec3{17.0F, 0.0F, 3.0F}, Vec3{29.0F, 0.0F, 3.0F}, PathFilter{}, corridor);
    CY_REQUIRE_FALSE(deferred.has_value());
    CY_CHECK_EQ(deferred.error().code, ErrorCode::Unavailable);

    // Forgetting it is the other operation, and it does drop the node.
    CY_REQUIRE(hierarchy.forget_tile(TileCoord{2, 0, 0}).has_value());
    CY_CHECK_EQ(hierarchy.live_region_count(), 3U);
}

CY_TEST_CASE("a rebuild recomputes one tile's regions and leaves its neighbours' alone") {
    // `navigation`: "WHEN navigation changes in one region THEN only that region's abstract
    // connections SHALL be recomputed."
    NavMesh mesh(allocator(), Name::intern("test.nav"), 8.0F);
    NavHierarchy hierarchy(allocator());
    publish_row(mesh, hierarchy, 4);

    const RegionId untouched =
        hierarchy.region_at(mesh, Vec3{1.0F, 0.0F, 3.0F}, Vec3{2.0F, 2.0F, 2.0F});
    const u32 polys_before = hierarchy.region(untouched)->polys;

    CY_REQUIRE(
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{2, 0, 0}, 8.0F, 4)).has_value());
    const Expected<HierarchyUpdate, Error> update =
        hierarchy.rebuild_tile(mesh, TileCoord{2, 0, 0});
    CY_REQUIRE(update.has_value());
    CY_CHECK_EQ(update->regions_removed, 1U);
    CY_CHECK_EQ(update->regions_added, 1U);
    // Sixteen polygons of ONE tile, not of four.
    CY_CHECK_EQ(update->polys_visited, 16U);
    CY_CHECK_EQ(hierarchy.live_region_count(), 4U);
    // The neighbour's region is the same object it was.
    CY_REQUIRE(hierarchy.region(untouched) != nullptr);
    CY_CHECK_EQ(hierarchy.region(untouched)->polys, polys_before);
}

CY_TEST_CASE("an off-mesh link is an abstract edge between two otherwise separate regions") {
    NavMesh mesh(allocator(), Name::intern("test.nav"), 8.0F);
    NavHierarchy hierarchy(allocator());
    CY_REQUIRE(
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{0, 0, 0}, 8.0F, 4)).has_value());
    CY_REQUIRE(
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{2, 0, 0}, 8.0F, 4)).has_value());

    NavLink jump;
    jump.from = Vec3{7.0F, 0.0F, 3.0F};
    jump.to = Vec3{17.0F, 0.0F, 3.0F};
    jump.action = Name::intern("Jump");
    jump.cost = 3.0F;
    CY_REQUIRE(mesh.add_link(jump, Vec3{2.0F, 2.0F, 2.0F}).has_value());

    CY_REQUIRE(hierarchy.rebuild_tile(mesh, TileCoord{0, 0, 0}).has_value());
    CY_REQUIRE(hierarchy.rebuild_tile(mesh, TileCoord{2, 0, 0}).has_value());

    const RegionId west = hierarchy.region_at(mesh, Vec3{1.0F, 0.0F, 3.0F}, Vec3{2.0F, 2.0F, 2.0F});
    const RegionId east =
        hierarchy.region_at(mesh, Vec3{22.0F, 0.0F, 3.0F}, Vec3{2.0F, 2.0F, 2.0F});
    CY_REQUIRE_NE(west, kInvalidRegion);
    CY_REQUIRE_NE(east, kInvalidRegion);
    CY_REQUIRE_EQ(hierarchy.edges_of(west).size(), usize{1});
    CY_CHECK_EQ(hierarchy.edges_of(west)[0].to, east);
    CY_CHECK_NEAR(hierarchy.edges_of(west)[0].cost, 3.0F, 0.001F);

    const Expected<AbstractPath, Error> plan = hierarchy.plan(west, east);
    CY_REQUIRE(plan.has_value());
    CY_CHECK(plan->found);
    CY_CHECK_EQ(plan->regions.size(), usize{2});
}
