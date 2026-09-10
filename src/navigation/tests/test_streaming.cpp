// Navigation streaming: tiles and regions loaded and unloaded as content streams, in-flight work
// failing cleanly, and navigation's tiling staying its own. M8.b task 6.1.
//
// THERE IS NO `NavStreamer` CLASS, AND THAT IS THE DESIGN. `navigation` asks for four properties,
// not for a component: tiles and regions load and unload with the world, the hierarchy is updated,
// in-flight queries remain valid or fail cleanly, and navigation retains its own tile layout. Each
// is a property of `NavMesh`, `NavHierarchy` and `update_agents` working together, and a class that
// owned all three would only be a place for a fifth policy to accumulate. This suite is what makes
// the four properties checkable, and it is where a `NavStreamer` would have had its tests anyway.
//
// What is NOT here, and is reported rather than implied: nothing consumes
// `world-partition-and-streaming`'s cell lifecycle events, and no cooked cell channel carries a
// navigation payload. Both are wiring into `cy::world`, and both are named in this milestone's
// handover.

#include <cy/core/memory/system_allocator.h>
#include <cy/navigation/components.h>
#include <cy/navigation/hierarchy.h>
#include <cy/test/test.h>

#include "nav_fixture.h"

using namespace cy;
using namespace cy::navigation;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

[[nodiscard]] ecs::WorldConfig config() noexcept {
    ecs::WorldConfig out;
    out.name = "navigation.streaming";
    return out;
}

/// Publish `count` tiles from x = 0 east, and register every one in the hierarchy.
void stream_in(NavMesh& mesh, NavHierarchy& hierarchy, u32 count) noexcept {
    for (u32 index = 0; index < count; ++index) {
        CY_REQUIRE(
            mesh.add_tile(testing::grid_tile(allocator(), TileCoord{static_cast<i32>(index), 0, 0},
                                             8.0F, 4))
                .has_value());
    }
    for (u32 index = 0; index < count; ++index) {
        CY_REQUIRE(
            hierarchy.rebuild_tile(mesh, TileCoord{static_cast<i32>(index), 0, 0}).has_value());
    }
}

}  // namespace

CY_TEST_CASE("a tile streams out and back in, and the hierarchy follows both ways") {
    NavMesh mesh(allocator(), Name::intern("test.nav"), 8.0F);
    NavHierarchy hierarchy(allocator());
    stream_in(mesh, hierarchy, 4);
    CY_CHECK_EQ(mesh.tile_count(), 4U);
    CY_CHECK_EQ(hierarchy.stats().regions, 4U);
    CY_CHECK_EQ(hierarchy.stats().resident_regions, 4U);

    // Out: the mesh loses the tile, the hierarchy keeps the region and marks it away.
    CY_REQUIRE(hierarchy.release_tile(TileCoord{1, 0, 0}).has_value());
    CY_REQUIRE(mesh.remove_tile(TileCoord{1, 0, 0}).has_value());
    CY_CHECK_EQ(mesh.tile_count(), 3U);
    CY_CHECK_EQ(hierarchy.stats().regions, 4U);
    CY_CHECK_EQ(hierarchy.stats().resident_regions, 3U);
    CY_CHECK_FALSE(mesh.tile_resident(TileCoord{1, 0, 0}));

    // Back in: the same coordinate, a new region, and the abstract graph reconnected.
    CY_REQUIRE(
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{1, 0, 0}, 8.0F, 4)).has_value());
    CY_REQUIRE(hierarchy.rebuild_tile(mesh, TileCoord{1, 0, 0}).has_value());
    CY_CHECK_EQ(hierarchy.stats().regions, 4U);
    CY_CHECK_EQ(hierarchy.stats().resident_regions, 4U);

    const RegionId west = hierarchy.region_at(mesh, Vec3{1.0F, 0.0F, 3.0F}, Vec3{2.0F, 2.0F, 2.0F});
    const RegionId east =
        hierarchy.region_at(mesh, Vec3{29.0F, 0.0F, 3.0F}, Vec3{2.0F, 2.0F, 2.0F});
    const Expected<AbstractPath, Error> replanned = hierarchy.plan(west, east);
    CY_REQUIRE(replanned.has_value());
    CY_CHECK(replanned->found);
    CY_CHECK(replanned->complete);
}

CY_TEST_CASE("a region unloading under an agent invalidates its path and triggers a repath") {
    // `navigation`: "WHEN a region unloads while an agent is pathing through it THEN the agent's
    // path SHALL be invalidated cleanly and a repath triggered, rather than dereferencing released
    // data."
    ecs::World world(allocator(), config());
    CY_REQUIRE(world.initialize().has_value());
    const Expected<NavComponents, Error> ids = NavComponents::register_all(world);
    CY_REQUIRE(ids.has_value());

    NavMesh mesh(allocator(), Name::intern("test.nav"), 8.0F);
    NavHierarchy hierarchy(allocator());
    stream_in(mesh, hierarchy, 4);
    PathQueue queue(allocator(), mesh, 1);

    const ComponentTypeId components[1] = {ids->agent};
    const Expected<Entity, Error> entity = world.create({components, 1});
    CY_REQUIRE(entity.has_value());
    NavAgent agent;
    agent.position = Vec3{1.0F, 0.0F, 3.0F};
    agent.target = Vec3{29.0F, 0.0F, 3.0F};
    CY_REQUIRE(world.set(*entity, ids->agent, agent).has_value());

    NavAgentReport report;
    CY_REQUIRE(update_agents(world, *ids, mesh, queue, 100, 1, report).has_value());
    CY_CHECK_EQ(report.repaths_issued, 1U);

    // The query completes and the agent starts following a corridor across the tile that is about
    // to go away.
    CY_CHECK_EQ(queue.update(101), 1U);
    PathCorridor corridor(allocator());
    PathResult result;
    CY_REQUIRE(queue.consume(world.get<NavAgent>(*entity, ids->agent)->query, corridor, result));
    CY_CHECK(result.found);
    CY_CHECK(corridor.valid_against(mesh));
    world.get_mut<NavAgent>(*entity, ids->agent)->status = NavPathStatus::Following;

    // The region streams out.
    CY_REQUIRE(hierarchy.release_tile(TileCoord{2, 0, 0}).has_value());
    CY_REQUIRE(mesh.remove_tile(TileCoord{2, 0, 0}).has_value());
    // Cleanly invalidated: the corridor answers false rather than dereferencing released data.
    CY_CHECK_FALSE(corridor.valid_against(mesh));

    // And the next pass repaths, rather than leaving the agent following a corridor that is gone.
    CY_REQUIRE(update_agents(world, *ids, mesh, queue, 102, 1, report).has_value());
    CY_CHECK_EQ(report.corridors_invalidated, 1U);
    CY_CHECK_EQ(report.repaths_issued, 1U);
}

CY_TEST_CASE("a path into unloaded content is abstract now and refined when it arrives") {
    // `navigation`: "the abstract path SHALL be produced and local refinement deferred until that
    // region streams in."
    NavMesh mesh(allocator(), Name::intern("test.nav"), 8.0F);
    NavHierarchy hierarchy(allocator());
    stream_in(mesh, hierarchy, 4);

    const RegionId from = hierarchy.region_at(mesh, Vec3{1.0F, 0.0F, 3.0F}, Vec3{2.0F, 2.0F, 2.0F});
    const RegionId to = hierarchy.region_at(mesh, Vec3{29.0F, 0.0F, 3.0F}, Vec3{2.0F, 2.0F, 2.0F});
    CY_REQUIRE(hierarchy.release_tile(TileCoord{3, 0, 0}).has_value());
    CY_REQUIRE(mesh.remove_tile(TileCoord{3, 0, 0}).has_value());

    const Expected<AbstractPath, Error> deferred = hierarchy.plan(from, to);
    CY_REQUIRE(deferred.has_value());
    CY_CHECK(deferred->found);
    CY_CHECK_FALSE(deferred->complete);
    CY_CHECK_EQ(deferred->first_unresident, 3U);

    PathCorridor corridor(allocator());
    const Expected<PathResult, Error> refused =
        hierarchy.refine(mesh, *deferred, 3, Vec3{25.0F, 0.0F, 3.0F}, Vec3{29.0F, 0.0F, 3.0F},
                         PathFilter{}, corridor);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::Unavailable);

    // It arrives, and the same segment refines.
    CY_REQUIRE(
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{3, 0, 0}, 8.0F, 4)).has_value());
    CY_REQUIRE(hierarchy.rebuild_tile(mesh, TileCoord{3, 0, 0}).has_value());
    const RegionId arrived =
        hierarchy.region_at(mesh, Vec3{29.0F, 0.0F, 3.0F}, Vec3{2.0F, 2.0F, 2.0F});
    const Expected<AbstractPath, Error> complete = hierarchy.plan(from, arrived);
    CY_REQUIRE(complete.has_value());
    CY_CHECK(complete->complete);
    corridor.clear();
    const Expected<PathResult, Error> refined =
        hierarchy.refine(mesh, *complete, 2, Vec3{17.0F, 0.0F, 3.0F}, Vec3{29.0F, 0.0F, 3.0F},
                         PathFilter{}, corridor);
    CY_REQUIRE(refined.has_value());
    CY_CHECK(refined->found);
}

CY_TEST_CASE("navigation tiling is its own, and no world cell size reaches it") {
    // `navigation`: "Navigation SHALL retain its OWN tile layout. Tile boundaries SHALL NOT be
    // required to match world cell boundaries", and "WHEN world cell size is changed THEN
    // navigation tiling SHALL be unaffected, since it owns its own layout."
    //
    // The check is structural rather than behavioural, and that is the strongest form available:
    // `coord_of` is a function of the mesh's own `tile_size()` and the position, and nothing in
    // `cy::navigation` names a world cell at all. Two meshes with different tile sizes over the
    // same geometry partition it differently and answer the same queries.
    NavMesh fine(allocator(), Name::intern("test.fine"), 8.0F);
    NavMesh coarse(allocator(), Name::intern("test.coarse"), 32.0F);
    CY_CHECK_EQ(fine.coord_of(Vec3{20.0F, 0.0F, 4.0F}).x, 2);
    CY_CHECK_EQ(coarse.coord_of(Vec3{20.0F, 0.0F, 4.0F}).x, 0);

    // The same square metre of ground, tiled two ways, is walkable in both.
    for (u32 index = 0; index < 4; ++index) {
        CY_REQUIRE(
            fine.add_tile(testing::grid_tile(allocator(), TileCoord{static_cast<i32>(index), 0, 0},
                                             8.0F, 4))
                .has_value());
    }
    CY_REQUIRE(coarse.add_tile(testing::grid_tile(allocator(), TileCoord{0, 0, 0}, 32.0F, 16))
                   .has_value());

    PathCorridor fine_path(allocator());
    PathCorridor coarse_path(allocator());
    const Vec3 start{1.0F, 0.0F, 3.0F};
    const Vec3 end{29.0F, 0.0F, 3.0F};
    const PathResult over_four =
        find_path(fine, start, end, Vec3{2.0F, 2.0F, 2.0F}, PathFilter{}, fine_path);
    const PathResult over_one =
        find_path(coarse, start, end, Vec3{2.0F, 2.0F, 2.0F}, PathFilter{}, coarse_path);
    CY_CHECK(over_four.found);
    CY_CHECK_FALSE(over_four.partial);
    CY_CHECK(over_one.found);
    CY_CHECK_FALSE(over_one.partial);
}
