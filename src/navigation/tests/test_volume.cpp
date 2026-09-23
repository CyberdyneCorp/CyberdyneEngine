// SPDX-License-Identifier: MIT
// Sparse 3D navigation and the shared surface/volume point-path contract.

#include <cy/core/memory/system_allocator.h>
#include <cy/navigation/debug.h>
#include <cy/navigation/volume.h>
#include <cy/test/test.h>

#include "nav_fixture.h"

using namespace cy;
using namespace cy::navigation;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

void canyon(NavVolume& volume, bool direct) noexcept {
    for (i32 x = 0; x <= 4; ++x) {
        if (x != 2 || direct) {
            const AreaType area = x == 2 ? AreaType{1} : kAreaGround;
            CY_REQUIRE(volume.set_cell({x, 1, 0}, area).has_value());
        }
    }
    for (i32 x = 1; x <= 3; ++x) {
        CY_REQUIRE(volume.set_cell({x, 2, 0}).has_value());
    }
}

}  // namespace

CY_TEST_CASE("a flying path climbs over a blocked canyon cell") {
    NavVolume volume(allocator(), 1.0F);
    canyon(volume, false);
    Array<Vec3> points(allocator());
    const Vec3 start = volume.center({0, 1, 0});
    const Vec3 end = volume.center({4, 1, 0});
    const PathResult result = volume.find_path(start, end, {}, PathFilter{}, points);
    CY_REQUIRE(result.found);
    CY_CHECK_FALSE(result.partial);
    CY_CHECK_GT(points.size(), usize{3});
    bool climbed = false;
    for (const Vec3 point : points.span()) {
        climbed = climbed || point.y > start.y;
    }
    CY_CHECK(climbed);
}

CY_TEST_CASE("volume cost mask budget and partial paths share surface query semantics") {
    NavVolume volume(allocator(), 1.0F);
    canyon(volume, true);
    Array<Vec3> points(allocator());
    const Vec3 start = volume.center({0, 1, 0});
    const Vec3 end = volume.center({4, 1, 0});

    PathFilter filter;
    filter.costs.multiplier[1] = 50.0F;
    PathResult result = volume.find_path(start, end, {}, filter, points);
    CY_REQUIRE(result.found);
    CY_CHECK_FALSE(result.partial);
    bool climbed = false;
    for (Vec3 point : points.span()) {
        climbed = climbed || point.y > start.y;
    }
    CY_CHECK(climbed);

    filter.areas &= ~area_bit(1);
    result = volume.find_path(start, end, {}, filter, points);
    CY_CHECK(result.found);
    CY_CHECK_FALSE(result.partial);

    filter.node_budget = 1;
    result = volume.find_path(start, end, {}, filter, points);
    CY_CHECK(result.found);
    CY_CHECK(result.partial);
    CY_CHECK(result.budget_exceeded);
    CY_CHECK_EQ(result.nodes_expanded, 1U);

    CY_REQUIRE(volume.remove_cell({2, 2, 0}).has_value());
    filter.node_budget = 2048;
    result = volume.find_path(start, end, {}, filter, points);
    CY_CHECK(result.found);
    CY_CHECK(result.partial);
    CY_CHECK_FALSE(result.budget_exceeded);

    // A blocked destination may be near another voxel, but it is still unreachable. Do not
    // silently claim a complete path and append the requested point inside the blockage.
    result =
        volume.find_path(start, volume.center({2, 1, 0}), Vec3{1.0F, 1.0F, 1.0F}, filter, points);
    CY_CHECK(result.found);
    CY_CHECK(result.partial);
}

CY_TEST_CASE("surface and volume share deterministic point-path delivery") {
    NavMesh mesh = testing::single_tile_mesh(allocator());
    NavVolume volume(allocator(), 1.0F);
    canyon(volume, false);
    SpatialPathQueue surface_queue(allocator(), NavigationSpace(mesh), 2);
    SpatialPathQueue volume_queue(allocator(), NavigationSpace(volume), 2);
    NavMetrics metrics;
    volume_queue.set_metrics(&metrics);
    const Vec3 surface_start{0.5F, 0.0F, 0.5F};
    const Vec3 surface_end{7.5F, 0.0F, 7.5F};
    const Vec3 volume_start = volume.center({0, 1, 0});
    const Vec3 volume_end = volume.center({4, 1, 0});

    const auto surface_id = surface_queue.submit(7, surface_start, surface_end,
                                                 Vec3{0.1F, 0.2F, 0.1F}, PathFilter{}, 10);
    const auto volume_id = volume_queue.submit(8, volume_start, volume_end, {}, PathFilter{}, 10);
    const auto second_volume_id =
        volume_queue.submit(9, volume_end, volume_start, {}, PathFilter{}, 10);
    CY_REQUIRE(surface_id.has_value());
    CY_REQUIRE(volume_id.has_value());
    CY_REQUIRE(second_volume_id.has_value());
    CY_CHECK_EQ(surface_queue.update(11), 0U);
    CY_CHECK_EQ(volume_queue.update(11), 0U);
    CY_CHECK_EQ(surface_queue.update(12), 1U);
    CY_CHECK_EQ(volume_queue.update(12), 2U);
    CY_REQUIRE_EQ(volume_queue.completed().size(), usize{2});
    CY_CHECK_EQ(volume_queue.completed()[0], *volume_id);
    CY_CHECK_EQ(volume_queue.completed()[1], *second_volume_id);
    CY_CHECK_EQ(surface_queue.owner(*surface_id), 7U);
    CY_CHECK_EQ(volume_queue.owner(*volume_id), 8U);

    Array<Vec3> surface_points(allocator());
    Array<Vec3> volume_points(allocator());
    PathResult surface_result;
    PathResult volume_result;
    CY_REQUIRE(surface_queue.consume(*surface_id, surface_points, surface_result));
    CY_REQUIRE(volume_queue.consume(*volume_id, volume_points, volume_result));
    CY_CHECK(surface_result.found);
    CY_CHECK(volume_result.found);
    CY_CHECK_FALSE(surface_result.partial);
    CY_CHECK_FALSE(volume_result.partial);
    CY_CHECK_GT(volume_points.size(), surface_points.size());
    CY_CHECK_EQ(metrics.snapshot().queries, 2U);
    CY_CHECK_GT(metrics.snapshot().path_length_mm, 0U);
}

CY_TEST_CASE("shared surface query stops a partial route on reachable ground") {
    NavMesh mesh = testing::single_tile_mesh(allocator());
    NavObstacleShape barrier;
    barrier.bounds = Aabb::from_min_max(Vec3{3.0F, -1.0F, 0.0F}, Vec3{5.0F, 1.0F, 8.0F});
    CY_REQUIRE(mesh.add_obstacle(barrier).has_value());

    NavigationSpace space(mesh);
    Array<Vec3> points(allocator());
    const Vec3 target{7.5F, 0.0F, 4.5F};
    const PathResult result = space.find_path(Vec3{0.5F, 0.0F, 4.5F}, target,
                                              Vec3{0.1F, 0.2F, 0.1F}, PathFilter{}, points);
    CY_REQUIRE(result.found);
    CY_CHECK(result.partial);
    CY_REQUIRE_FALSE(points.empty());
    CY_CHECK_LT(points[points.size() - 1].x, 3.0F);
}
