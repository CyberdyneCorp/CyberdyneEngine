// Generating a tile from source geometry, over BOTH back ends. M8.b task 6.1.
//
// INTEGRATION, and deliberately: a case here voxelises a sixteen-metre tile at a quarter-metre cell
// twice over, which is four thousand columns and eight thousand triangles per build. The subject IS
// the voxelisation, so making it fit the unit tier's millisecond would mean measuring something
// else. Hard rule 7 of this milestone's brief, and the taxonomy in `testing-and-quality`.
//
// The point of this suite is the comparison. `navigation` fixes what generation must do — erode by
// the agent radius, reject a slope, honour the declared filters — and build.h provides two
// implementations of it. Every case here asks the same question of both, so a difference between
// them is a failure here rather than a surprise in whichever configuration nobody built.

#include <cy/core/memory/system_allocator.h>
#include <cy/navigation/build.h>
#include <cy/navigation/query.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::navigation;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

constexpr f32 kExtent = 16.0F;
constexpr u32 kCells = 16;
constexpr f32 kStep = kExtent / static_cast<f32>(kCells);

/// A flat 16 m square of triangles at y = 0, with an optional square hole and an optional
/// near-vertical face. One helper so every case builds the same world and differs in one respect.
struct SourceMesh {
    Array<Vec3> vertices;
    Array<u32> indices;
    Array<u8> layer;
    Array<u8> tag;
    Array<AreaType> area;

    explicit SourceMesh(Allocator& alloc) noexcept
        : vertices(alloc), indices(alloc), layer(alloc), tag(alloc), area(alloc) {}

    [[nodiscard]] NavSourceGeometry geometry() const noexcept {
        return NavSourceGeometry{vertices.span(), indices.span(), layer.span(), tag.span(),
                                 area.span()};
    }
};

void add_triangle(SourceMesh& mesh, Vec3 a, Vec3 b, Vec3 c, u8 layer, u8 tag,
                  AreaType area) noexcept {
    const u32 base = static_cast<u32>(mesh.vertices.size());
    CY_REQUIRE(mesh.vertices.push_back(a).has_value());
    CY_REQUIRE(mesh.vertices.push_back(b).has_value());
    CY_REQUIRE(mesh.vertices.push_back(c).has_value());
    for (u32 offset = 0; offset < 3; ++offset) {
        CY_REQUIRE(mesh.indices.push_back(base + offset).has_value());
    }
    CY_REQUIRE(mesh.layer.push_back(layer).has_value());
    CY_REQUIRE(mesh.tag.push_back(tag).has_value());
    CY_REQUIRE(mesh.area.push_back(area).has_value());
}

/// `hole` is a cell range that contributes no geometry, so the erosion has something to erode
/// against.
void add_ground(SourceMesh& mesh, u32 hole_first, u32 hole_last, u8 layer, u8 tag) noexcept {
    for (u32 row = 0; row < kCells; ++row) {
        for (u32 column = 0; column < kCells; ++column) {
            const bool in_hole = row >= hole_first && row <= hole_last && column >= hole_first &&
                                 column <= hole_last;
            if (in_hole) {
                continue;
            }
            const f32 x0 = static_cast<f32>(column) * kStep;
            const f32 x1 = x0 + kStep;
            const f32 z0 = static_cast<f32>(row) * kStep;
            const f32 z1 = z0 + kStep;
            // Wound so the normal points up: `cross(b - a, c - a).y > 0`. A ground plane wound the
            // other way is a ceiling, and every triangle of it is "steeper than the limit".
            add_triangle(mesh, Vec3{x0, 0.0F, z0}, Vec3{x1, 0.0F, z1}, Vec3{x1, 0.0F, z0}, layer,
                         tag, kAreaGround);
            add_triangle(mesh, Vec3{x0, 0.0F, z0}, Vec3{x0, 0.0F, z1}, Vec3{x1, 0.0F, z1}, layer,
                         tag, kAreaGround);
        }
    }
}

[[nodiscard]] Aabb tile_bounds() noexcept {
    return Aabb::from_min_max(Vec3{0.0F, -1.0F, 0.0F}, Vec3{kExtent, 3.0F, kExtent});
}

[[nodiscard]] NavBuildParams params(NavBuildBackend backend, f32 radius) noexcept {
    NavBuildParams out;
    out.backend = backend;
    out.cell_size = 0.25F;
    out.cell_height = 0.2F;
    out.agent_radius = radius;
    out.agent_height = 2.0F;
    out.agent_max_climb = 0.4F;
    out.agent_max_slope_degrees = 45.0F;
    return out;
}

/// Both back ends. Every case asserts `report.backend` beside its own subject, so a failure says
/// which iteration produced it without a message the framework would print as an address.
struct Backend {
    NavBuildBackend value;
};

[[nodiscard]] bool on_mesh(const NavMesh& mesh, Vec3 point) noexcept {
    Vec3 nearest;
    return mesh.find_nearest(point, Vec3{0.4F, 1.0F, 0.4F}, kAllAreas, nearest).valid();
}

[[nodiscard]] NavMesh publish(NavTileData&& tile) noexcept {
    NavMesh mesh(allocator(), Name::intern("test.build"), kExtent);
    CY_REQUIRE(mesh.add_tile(std::move(tile)).has_value());
    return mesh;
}

}  // namespace

CY_TEST_CASE(
    "this build has Recast, and asking for it without it is refused rather than downgraded") {
    // The engine's own back end is always there; Recast is there when `CY_NAVIGATION` is on, which
    // is its default. Whichever this build is, the two branches below are the whole contract.
    NavBuildReport report;
    SourceMesh source(allocator());
    add_ground(source, kCells, kCells, 0, 0);

    const Expected<NavTileData, Error> forced =
        build_tile(allocator(), params(NavBuildBackend::Recast, 0.3F), source.geometry(),
                   TileCoord{0, 0, 0}, tile_bounds(), report);
    if (recast_available()) {
        CY_CHECK(forced.has_value());
        CY_CHECK_EQ(report.backend, NavBuildBackend::Recast);
    } else {
        CY_REQUIRE_FALSE(forced.has_value());
        CY_CHECK_EQ(forced.error().code, ErrorCode::Unsupported);
    }

    // `Automatic` never fails for want of a back end.
    const Expected<NavTileData, Error> automatic =
        build_tile(allocator(), params(NavBuildBackend::Automatic, 0.3F), source.geometry(),
                   TileCoord{0, 0, 0}, tile_bounds(), report);
    CY_CHECK(automatic.has_value());
    CY_CHECK_EQ(report.backend,
                recast_available() ? NavBuildBackend::Recast : NavBuildBackend::Engine);
}

CY_TEST_CASE("a flat square becomes a walkable tile a path crosses, on either back end") {
    Backend backends[2] = {{NavBuildBackend::Engine}, {NavBuildBackend::Recast}};
    for (const Backend& backend : backends) {
        if (backend.value == NavBuildBackend::Recast && !recast_available()) {
            continue;
        }
        NavBuildReport report;
        SourceMesh source(allocator());
        add_ground(source, kCells, kCells, 0, 0);
        Expected<NavTileData, Error> tile =
            build_tile(allocator(), params(backend.value, 0.3F), source.geometry(),
                       TileCoord{0, 0, 0}, tile_bounds(), report);
        CY_REQUIRE(tile.has_value());
        CY_CHECK_EQ(report.backend, backend.value);
        CY_CHECK_EQ(report.triangles_in, kCells * kCells * 2);
        CY_CHECK_EQ(report.triangles_filtered, 0U);
        CY_CHECK_EQ(report.triangles_steep, 0U);
        CY_CHECK_GT(report.polys, 0U);

        NavMesh mesh = publish(std::move(*tile));
        CY_CHECK(on_mesh(mesh, Vec3{8.0F, 0.0F, 8.0F}));

        PathCorridor corridor(allocator());
        const PathResult result = find_path(mesh, Vec3{2.0F, 0.0F, 2.0F}, Vec3{14.0F, 0.0F, 14.0F},
                                            Vec3{1.0F, 2.0F, 1.0F}, PathFilter{}, corridor);
        CY_CHECK(result.found);
        CY_CHECK_FALSE(result.partial);
    }
}

CY_TEST_CASE("the agent radius erodes the walkable surface away from a hole, on either back end") {
    // `navigation`: "WHEN the agent radius is 0.5 m THEN the walkable surface SHALL be shrunk by
    // that distance from obstacles, so a path's centre line is always traversable."
    //
    // The hole spans cells 6..9, which is x and z in [6, 10). A point at 5.5 is half a metre from
    // its western edge: inside the mesh for a thin agent, outside it for a wide one.
    Backend backends[2] = {{NavBuildBackend::Engine}, {NavBuildBackend::Recast}};
    for (const Backend& backend : backends) {
        if (backend.value == NavBuildBackend::Recast && !recast_available()) {
            continue;
        }
        NavBuildReport thin_report;
        NavBuildReport wide_report;
        SourceMesh source(allocator());
        add_ground(source, 6, 9, 0, 0);

        Expected<NavTileData, Error> thin =
            build_tile(allocator(), params(backend.value, 0.05F), source.geometry(),
                       TileCoord{0, 0, 0}, tile_bounds(), thin_report);
        Expected<NavTileData, Error> wide =
            build_tile(allocator(), params(backend.value, 1.5F), source.geometry(),
                       TileCoord{0, 0, 0}, tile_bounds(), wide_report);
        CY_REQUIRE(thin.has_value());
        CY_REQUIRE(wide.has_value());
        CY_CHECK_EQ(thin_report.backend, backend.value);
        CY_CHECK_EQ(wide_report.backend, backend.value);

        NavMesh thin_mesh = publish(std::move(*thin));
        NavMesh wide_mesh = publish(std::move(*wide));
        CY_CHECK(on_mesh(thin_mesh, Vec3{5.5F, 0.0F, 8.0F}));
        CY_CHECK_FALSE(on_mesh(wide_mesh, Vec3{5.5F, 0.0F, 8.0F}));
        // The hole itself is outside both.
        CY_CHECK_FALSE(on_mesh(thin_mesh, Vec3{8.0F, 0.0F, 8.0F}));
        // Far from the hole, the wide agent still has ground.
        CY_CHECK(on_mesh(wide_mesh, Vec3{2.0F, 0.0F, 2.0F}));
    }
}

CY_TEST_CASE("a face steeper than the slope limit contributes nothing, on either back end") {
    Backend backends[2] = {{NavBuildBackend::Engine}, {NavBuildBackend::Recast}};
    for (const Backend& backend : backends) {
        if (backend.value == NavBuildBackend::Recast && !recast_available()) {
            continue;
        }
        SourceMesh source(allocator());
        add_ground(source, kCells, kCells, 0, 0);
        // A near-vertical shelf over the ground at (12, 12): far too steep to stand on, and high
        // enough that it is not a step.
        add_triangle(source, Vec3{12.0F, 0.0F, 12.0F}, Vec3{13.0F, 0.0F, 12.0F},
                     Vec3{12.02F, 2.5F, 12.0F}, 0, 0, kAreaGround);

        NavBuildReport report;
        Expected<NavTileData, Error> tile =
            build_tile(allocator(), params(backend.value, 0.3F), source.geometry(),
                       TileCoord{0, 0, 0}, tile_bounds(), report);
        CY_REQUIRE(tile.has_value());
        CY_CHECK_EQ(report.backend, backend.value);
        CY_CHECK_EQ(report.triangles_steep, 1U);
        NavMesh mesh = publish(std::move(*tile));
        // Nothing stands 2.5 m up: the shelf produced no polygon.
        Vec3 nearest;
        CY_CHECK_FALSE(
            mesh.find_nearest(Vec3{12.0F, 2.5F, 12.0F}, Vec3{0.3F, 0.3F, 0.3F}, kAllAreas, nearest)
                .valid());
    }
}

CY_TEST_CASE("layer, tag and volume filters drop triangles before either back end sees them") {
    SourceMesh source(allocator());
    add_ground(source, kCells, kCells, 0, 0);
    // A second, higher floor on layer 1 and tag 2.
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            const f32 x0 = static_cast<f32>(column) * kStep;
            const f32 z0 = static_cast<f32>(row) * kStep;
            add_triangle(source, Vec3{x0, 2.5F, z0}, Vec3{x0 + kStep, 2.5F, z0 + kStep},
                         Vec3{x0 + kStep, 2.5F, z0}, 1, 2, kAreaGround);
            add_triangle(source, Vec3{x0, 2.5F, z0}, Vec3{x0, 2.5F, z0 + kStep},
                         Vec3{x0 + kStep, 2.5F, z0 + kStep}, 1, 2, kAreaGround);
        }
    }
    const u32 upper = 4 * 4 * 2;

    NavBuildParams by_layer = params(NavBuildBackend::Engine, 0.3F);
    by_layer.layers = u64{1};  // layer 0 only
    NavBuildReport report;
    CY_REQUIRE(build_tile(allocator(), by_layer, source.geometry(), TileCoord{0, 0, 0},
                          tile_bounds(), report)
                   .has_value());
    CY_CHECK_EQ(report.triangles_filtered, upper);

    NavBuildParams by_tag = params(NavBuildBackend::Engine, 0.3F);
    by_tag.tags = u64{1};  // tag 0 only
    CY_REQUIRE(build_tile(allocator(), by_tag, source.geometry(), TileCoord{0, 0, 0}, tile_bounds(),
                          report)
                   .has_value());
    CY_CHECK_EQ(report.triangles_filtered, upper);

    const Aabb exclude = Aabb::from_min_max(Vec3{-1.0F, 2.0F, -1.0F}, Vec3{20.0F, 4.0F, 20.0F});
    NavBuildParams by_volume = params(NavBuildBackend::Engine, 0.3F);
    by_volume.exclude_volumes = Span<const Aabb>(&exclude, 1);
    CY_REQUIRE(build_tile(allocator(), by_volume, source.geometry(), TileCoord{0, 0, 0},
                          tile_bounds(), report)
                   .has_value());
    CY_CHECK_EQ(report.triangles_filtered, upper);

    // And with no filter at all, nothing is dropped.
    CY_REQUIRE(build_tile(allocator(), params(NavBuildBackend::Engine, 0.3F), source.geometry(),
                          TileCoord{0, 0, 0}, tile_bounds(), report)
                   .has_value());
    CY_CHECK_EQ(report.triangles_filtered, 0U);
}

CY_TEST_CASE("a degenerate request is refused with a diagnostic rather than allocated") {
    SourceMesh source(allocator());
    add_ground(source, kCells, kCells, 0, 0);
    NavBuildReport report;

    NavBuildParams zero_cell = params(NavBuildBackend::Engine, 0.3F);
    zero_cell.cell_size = 0.0F;
    const Expected<NavTileData, Error> no_cell = build_tile(
        allocator(), zero_cell, source.geometry(), TileCoord{0, 0, 0}, tile_bounds(), report);
    CY_REQUIRE_FALSE(no_cell.has_value());
    CY_CHECK_EQ(no_cell.error().code, ErrorCode::InvalidArgument);

    NavBuildParams tiny_cell = params(NavBuildBackend::Engine, 0.3F);
    tiny_cell.cell_size = 0.0005F;
    const Expected<NavTileData, Error> too_many = build_tile(
        allocator(), tiny_cell, source.geometry(), TileCoord{0, 0, 0}, tile_bounds(), report);
    CY_REQUIRE_FALSE(too_many.has_value());
    CY_CHECK_EQ(too_many.error().code, ErrorCode::OutOfRange);
}
