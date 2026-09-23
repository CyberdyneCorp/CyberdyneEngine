// A tilemap's authored polygons are rebuilt per chunk and searched on the same polygon graph.

#include <cy/core/memory/system_allocator.h>
#include <cy/navigation/navigation2d.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::navigation;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

[[nodiscard]] NavPolygon2D square() noexcept {
    NavPolygon2D polygon;
    polygon.corners[0] = Vec2{0.0F, 0.0F};
    polygon.corners[1] = Vec2{1.0F, 0.0F};
    polygon.corners[2] = Vec2{1.0F, 1.0F};
    polygon.corners[3] = Vec2{0.0F, 1.0F};
    polygon.corner_count = 4;
    return polygon;
}

}  // namespace

CY_TEST_CASE("tilemap path detours around a blocked cell") {
    NavMesh2D mesh(allocator(), Name::intern("test.tilemap"), 2, 1.0F);
    const NavPolygon2D polygon = square();
    const Span<const NavPolygon2D> navigation(&polygon, 1);
    const TilemapNavCell all_cells[4] = {
        {0, 0, navigation}, {1, 0, navigation}, {0, 1, navigation}, {1, 1, navigation}};
    CY_REQUIRE(mesh.rebuild_chunk(TileCoord{0, 0, 0}, all_cells).has_value());
    CY_REQUIRE(mesh.rebuild_chunk(TileCoord{1, 0, 0}, all_cells).has_value());
    CY_CHECK_EQ(mesh.chunk_count(), 2U);

    PathCorridor corridor(allocator());
    const Vec2 start{0.25F, 0.25F};
    const Vec2 target{3.75F, 0.25F};
    const PathResult before =
        mesh.find_path(start, target, Vec2{0.5F, 0.5F}, PathFilter{}, corridor);
    CY_REQUIRE(before.found);
    CY_CHECK_FALSE(before.partial);
    const PolyRef first_chunk_poly = corridor.polys()[0];

    // Remove the upper-left cell in the second chunk. The lower row still joins the chunks, so
    // the path must turn down around the hole; a full-mesh rebuild would stale first_chunk_poly.
    const TilemapNavCell changed_cells[3] = {
        {1, 0, navigation}, {0, 1, navigation}, {1, 1, navigation}};
    CY_REQUIRE(mesh.rebuild_chunk(TileCoord{1, 0, 0}, changed_cells).has_value());
    CY_CHECK_EQ(mesh.chunk_count(), 2U);
    CY_CHECK_NE(mesh.version(), 0U);

    corridor.clear();
    const PathResult after =
        mesh.find_path(start, target, Vec2{0.5F, 0.5F}, PathFilter{}, corridor);
    CY_REQUIRE(after.found);
    CY_CHECK_FALSE(after.partial);
    CY_REQUIRE_FALSE(corridor.empty());
    CY_CHECK_EQ(corridor.polys()[0], first_chunk_poly);

    Array<Vec2> points(allocator());
    CY_REQUIRE(mesh.straighten(corridor, start, target, points).has_value());
    CY_REQUIRE(points.size() >= usize{3});
    if (points.size() > 2) {
        CY_CHECK_GE(points[2].y, 1.0F);
    }
    CY_CHECK_EQ(points[0].x, start.x);
    CY_CHECK_EQ(points[points.size() - 1].x, target.x);
}

CY_TEST_CASE("an empty tilemap chunk removes only its own navigation") {
    NavMesh2D mesh(allocator(), Name::intern("test.tilemap"), 2, 1.0F);
    const NavPolygon2D polygon = square();
    const TilemapNavCell cell{0, 0, Span<const NavPolygon2D>(&polygon, 1)};
    CY_REQUIRE(
        mesh.rebuild_chunk(TileCoord{0, 0, 0}, Span<const TilemapNavCell>(&cell, 1)).has_value());
    CY_REQUIRE(
        mesh.rebuild_chunk(TileCoord{1, 0, 0}, Span<const TilemapNavCell>(&cell, 1)).has_value());
    CY_REQUIRE(mesh.rebuild_chunk(TileCoord{1, 0, 0}, {}).has_value());
    CY_CHECK_EQ(mesh.chunk_count(), 1U);

    PathCorridor corridor(allocator());
    const PathResult result = mesh.find_path(Vec2{0.25F, 0.25F}, Vec2{2.25F, 0.25F},
                                             Vec2{0.4F, 0.4F}, PathFilter{}, corridor);
    CY_CHECK_FALSE(result.found);
}

CY_TEST_CASE("2D collision polygons conservatively remove covered navigation cells") {
    NavMesh2D mesh(allocator(), Name::intern("test.collision2d"), 4, 1.0F);
    const Vec2 vertices[4] = {{1.0F, 1.0F}, {3.0F, 1.0F}, {3.0F, 2.0F}, {1.0F, 2.0F}};
    const CollisionPolygon2D blocker{Span<const Vec2>(vertices, 4)};
    CY_REQUIRE(mesh.rebuild_chunk_from_collision(TileCoord{0, 0, 0},
                                                 Span<const CollisionPolygon2D>(&blocker, 1))
                   .has_value());

    PathCorridor corridor(allocator());
    const Vec2 start{0.25F, 1.25F};
    const Vec2 target{3.75F, 1.25F};
    const PathResult path =
        mesh.find_path(start, target, Vec2{0.25F, 0.25F}, PathFilter{}, corridor);
    CY_REQUIRE(path.found);
    CY_CHECK_FALSE(path.partial);
    Array<Vec2> points(allocator());
    CY_REQUIRE(mesh.straighten(corridor, start, target, points).has_value());
    CY_CHECK_GT(points.size(), usize{2});

    corridor.clear();
    const PathResult inside =
        mesh.find_path(start, Vec2{1.5F, 1.5F}, Vec2{0.1F, 0.1F}, PathFilter{}, corridor);
    CY_CHECK_FALSE(inside.found);
}
