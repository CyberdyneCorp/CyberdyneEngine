// Flow fields: one field for many agents, incremental regeneration, reference counting and
// determinism. M8.b task 6.2.
//
// INTEGRATION, and deliberately: the determinism case integrates a Dijkstra field over a hundred
// and twenty-eight cells TWICE and compares every cell of the two, and the cache case builds three
// fields. The subject is the integration, so making it fit the unit tier's millisecond would mean
// measuring something else. Hard rule 7 of this milestone's brief.

#include <cy/core/memory/system_allocator.h>
#include <cy/navigation/flow_field.h>
#include <cy/test/test.h>

#include "nav_fixture.h"

using namespace cy;
using namespace cy::navigation;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

[[nodiscard]] FlowFieldParams params() noexcept {
    FlowFieldParams p;
    p.region = Aabb{Vec3{0.0F, -1.0F, 0.0F}, Vec3{16.0F, 1.0F, 8.0F}};
    p.cell_size = 1.0F;
    p.sample_height = 2.0F;
    return p;
}

[[nodiscard]] NavMesh two_tile_mesh() noexcept {
    NavMesh mesh(allocator(), Name::intern("test.nav"), 8.0F);
    const Expected<TileChange, Error> first =
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{0, 0, 0}, 8.0F, 8));
    CY_REQUIRE(first.has_value());
    const Expected<TileChange, Error> second =
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{1, 0, 0}, 8.0F, 8));
    CY_REQUIRE(second.has_value());
    return mesh;
}

}  // namespace

CY_TEST_CASE("a field points every navigable cell toward the destination") {
    NavMesh mesh = two_tile_mesh();
    FlowField field(allocator(), params());
    const Vec3 destination{15.0F, 0.0F, 4.0F};
    const Expected<FlowFieldUpdate, Error> built = field.build(mesh, {&destination, 1});
    CY_REQUIRE(built.has_value());
    CY_CHECK(built->full_rebuild);
    CY_CHECK_GT(built->cells_visited, 0U);
    CY_CHECK_EQ(field.mesh_version(), mesh.version());

    CY_CHECK(field.reachable_at(Vec3{1.0F, 0.0F, 4.0F}));
    const Vec3 near_start = field.direction_at(Vec3{1.0F, 0.0F, 4.0F});
    // Downhill in integrated cost is eastward, toward the destination.
    CY_CHECK_GT(near_start.x, 0.5F);
    CY_CHECK_GT(field.cost_at(Vec3{1.0F, 0.0F, 4.0F}), field.cost_at(Vec3{13.0F, 0.0F, 4.0F}));
}

CY_TEST_CASE("flow-field generation is deterministic") {
    NavMesh mesh = two_tile_mesh();
    const Vec3 destination{15.0F, 0.0F, 4.0F};

    FlowField first(allocator(), params());
    FlowField second(allocator(), params());
    CY_REQUIRE(first.build(mesh, {&destination, 1}).has_value());
    CY_REQUIRE(second.build(mesh, {&destination, 1}).has_value());

    for (u32 cell = 0; cell < first.cell_count(); ++cell) {
        const Vec3 centre = first.cell_centre(cell);
        CY_REQUIRE_EQ(first.cost_at(centre), second.cost_at(centre));
        CY_REQUIRE_EQ(first.direction_at(centre).x, second.direction_at(centre).x);
        CY_REQUIRE_EQ(first.direction_at(centre).z, second.direction_at(centre).z);
    }
}

CY_TEST_CASE("regeneration touches a fraction of the field") {
    NavMesh mesh = two_tile_mesh();
    FlowField field(allocator(), params());
    const Vec3 destination{15.0F, 0.0F, 4.0F};
    const Expected<FlowFieldUpdate, Error> built = field.build(mesh, {&destination, 1});
    CY_REQUIRE(built.has_value());

    Vec3 nearest;
    const PolyRef under =
        mesh.find_nearest(Vec3{8.0F, 0.0F, 2.0F}, Vec3{1.0F, 2.0F, 1.0F}, kAllAreas, nearest);
    CY_REQUIRE(under.valid());
    NavObstacleShape crate;
    crate.centre = nearest;
    crate.radius = 1.0F;
    crate.height = 2.0F;
    const Expected<ObstacleId, Error> id = mesh.add_obstacle(crate);
    CY_REQUIRE(id.has_value());

    const Aabb dirty{Vec3{6.0F, -1.0F, 0.0F}, Vec3{10.0F, 1.0F, 4.0F}};
    const Expected<FlowFieldUpdate, Error> patched = field.regenerate(mesh, dirty);
    CY_REQUIRE(patched.has_value());
    CY_CHECK_FALSE(patched->full_rebuild);
    // `navigation`: "the affected region of the field SHALL be regenerated incrementally rather
    // than the whole field recomputed" — a measurement, not a claim.
    CY_CHECK_LT(patched->cells_visited, built->cells_visited);
}

CY_TEST_CASE("a cache shares one field and releases it when the last user leaves") {
    NavMesh mesh = two_tile_mesh();
    FlowFieldCache cache(allocator(), params());

    const Expected<FlowField*, Error> first = cache.acquire(mesh, Vec3{15.0F, 0.0F, 4.0F});
    const Expected<FlowField*, Error> second = cache.acquire(mesh, Vec3{15.0F, 0.0F, 4.0F});
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    // Twenty thousand agents ordered to one place share one field, not twenty thousand queries.
    CY_CHECK_EQ(*first, *second);
    CY_CHECK_EQ(cache.size(), 1U);
    CY_CHECK_EQ((*first)->users(), 2U);

    const Expected<FlowField*, Error> other = cache.acquire(mesh, Vec3{1.0F, 0.0F, 1.0F});
    CY_REQUIRE(other.has_value());
    CY_CHECK_EQ(cache.size(), 2U);

    cache.release(*first);
    cache.release(*second);
    CY_CHECK_EQ(cache.collect(), 1U);
    CY_CHECK_EQ(cache.size(), 1U);
}
