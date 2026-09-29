// SPDX-License-Identifier: MIT
// The tiling loop above `build_tile`. See cy/navigation/bake.h.

#include <cy/navigation/bake.h>
#include <cy/navigation/tile_identity.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace cy::navigation {
namespace {

/// `build_tile`'s own ceiling on voxel columns per tile.
constexpr f32 kMaxCellsPerTile = 4.0F * 1024.0F * 1024.0F;
/// The most tiles one call visits. A region larger than this is a mistake in its bounds.
constexpr i64 kMaxTilesPerRegion = 65536;
/// Tile coordinates stay far inside `i32` so `x * tile_size` never overflows a float's integers.
constexpr f32 kMaxTileIndex = 1073741824.0F;

[[nodiscard]] bool positive(f32 value) noexcept {
    return std::isfinite(value) && value > 0.0F;
}

/// The Y range of the source geometry, or [0, 0] when there is none.
struct VerticalRange {
    f32 low = 0.0F;
    f32 high = 0.0F;
};

[[nodiscard]] VerticalRange vertical_range(const NavSourceGeometry& geometry) noexcept {
    if (geometry.vertices.empty()) {
        return {};
    }
    VerticalRange range{geometry.vertices[0].y, geometry.vertices[0].y};
    for (const Vec3& vertex : geometry.vertices) {
        range.low = std::fmin(range.low, vertex.y);
        range.high = std::fmax(range.high, vertex.y);
    }
    return range;
}

[[nodiscard]] Aabb tile_box(const NavBakeSettings& settings, VerticalRange range,
                            TileCoord coord) noexcept {
    const f32 size = settings.tile_size;
    const f32 x = static_cast<f32>(coord.x) * size;
    const f32 z = static_cast<f32>(coord.z) * size;
    return Aabb::from_min_max(Vec3{x, range.low - settings.step_height - settings.cell_height, z},
                              Vec3{x + size, range.high + settings.agent_height, z + size});
}

[[nodiscard]] bool triangle_centroid(const NavSourceGeometry& geometry, usize triangle,
                                     Vec3& centroid) noexcept {
    const usize count = geometry.vertices.size();
    const u32 i0 = geometry.indices[(triangle * 3) + 0];
    const u32 i1 = geometry.indices[(triangle * 3) + 1];
    const u32 i2 = geometry.indices[(triangle * 3) + 2];
    if (i0 >= count || i1 >= count || i2 >= count) {
        return false;
    }
    centroid =
        (geometry.vertices[i0] + geometry.vertices[i1] + geometry.vertices[i2]) * (1.0F / 3.0F);
    return true;
}

/// The area of the volume with the highest node that contains `point`, or `base`.
[[nodiscard]] AreaType winning_area(Span<const NavAreaVolume> areas, Vec3 point,
                                    AreaType base) noexcept {
    AreaType area = base;
    u64 best_node = 0;
    bool found = false;
    for (const NavAreaVolume& volume : areas) {
        if (volume.bounds.contains(point) && (!found || volume.node >= best_node)) {
            area = volume.area;
            best_node = volume.node;
            found = true;
        }
    }
    return area;
}

/// What every tile of one bake shares: the parameters, the geometry with its assigned areas, and
/// the vertical range. Built once per call.
struct BakeInputs {
    NavBuildParams params;
    NavSourceGeometry geometry;
    VerticalRange range;
    Array<AreaType> areas;
    Array<Aabb> excludes;

    explicit BakeInputs(Allocator& allocator) noexcept : areas(allocator), excludes(allocator) {}
};

[[nodiscard]] Status prepare(const NavBakeSettings& settings, const NavBakeSource& source,
                             const NavMesh& mesh, BakeInputs& inputs) noexcept {
    if (Status valid = validate_bake_settings(settings); !valid) {
        return valid;
    }
    if (mesh.tile_size() != settings.tile_size) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "the mesh's tile size differs from the bake settings'"});
    }
    if (Status assigned = assign_triangle_areas(source, inputs.areas); !assigned) {
        return assigned;
    }
    for (const NavSurfaceVolume& surface : source.surfaces) {
        if (surface.exclude) {
            if (Status pushed = inputs.excludes.push_back(surface.bounds); !pushed) {
                return pushed;
            }
        }
    }
    inputs.params = build_params(settings);
    inputs.params.exclude_volumes = inputs.excludes.span();
    inputs.geometry = source.geometry;
    inputs.geometry.area = inputs.areas.span();
    inputs.range = vertical_range(source.geometry);
    return ok();
}

/// The source counters describe the whole source (every tile filters all of it); the output
/// counters and the time are summed.
void accumulate(NavBuildReport& totals, const NavBuildReport& tile) noexcept {
    totals.triangles_in = tile.triangles_in;
    totals.triangles_filtered = tile.triangles_filtered;
    totals.triangles_steep = tile.triangles_steep;
    totals.spans += tile.spans;
    totals.polys += tile.polys;
    totals.vertices += tile.vertices;
    totals.duration_ns += tile.duration_ns;
    totals.backend = tile.backend;
}

/// Build one tile and publish it, or remove the occupant when it came out empty.
[[nodiscard]] Status bake_one(Allocator& allocator, const NavBakeSettings& settings,
                              const BakeInputs& inputs, TileCoord coord, NavMesh& mesh,
                              NavBakeReport& report, NavBakeTile& baked) noexcept {
    NavBuildReport built_report;
    Expected<NavTileData, Error> built =
        build_tile(allocator, inputs.params, inputs.geometry, coord,
                   tile_box(settings, inputs.range, coord), built_report);
    if (!built) {
        return make_unexpected(built.error());
    }
    accumulate(report.totals, built_report);
    baked.coord = coord;
    baked.polys = static_cast<u32>(built->polys().size());
    baked.empty = baked.polys == 0;
    baked.digest = tile_digest(*built);
    if (baked.empty) {
        ++report.tiles_empty;
        return mesh.tile_resident(coord) ? mesh.remove_tile(coord) : ok();
    }
    ++report.tiles_built;
    Expected<TileChange, Error> published = mesh.add_tile(std::move(*built));
    return published ? ok() : make_unexpected(published.error());
}

[[nodiscard]] Expected<NavBakeReport, Error> bake_coords(
    Allocator& allocator, const NavBakeSettings& settings, const BakeInputs& inputs,
    Span<const TileCoord> coords, NavMesh& mesh, NavBakeObserver* observer) noexcept {
    NavBakeReport report(allocator);
    report.totals.backend = settings.backend;
    const u32 total = static_cast<u32>(coords.size());
    for (u32 index = 0; index < total; ++index) {
        NavBakeTile baked;
        if (Status done = bake_one(allocator, settings, inputs, coords[index], mesh, report, baked);
            !done) {
            return make_unexpected(done.error());
        }
        if (Status listed = report.tiles.push_back(baked); !listed) {
            return make_unexpected(listed.error());
        }
        if (observer != nullptr &&
            observer->tile_baked(index + 1, total, baked) == NavBakeProgress::Cancel) {
            report.cancelled = index + 1 < total;
            break;
        }
    }
    return report;
}

[[nodiscard]] bool listed(Span<const TileCoord> coords, TileCoord coord) noexcept {
    return std::ranges::any_of(coords,
                               [coord](const TileCoord& candidate) { return candidate == coord; });
}

[[nodiscard]] Status remove_outside(NavMesh& mesh, Span<const TileCoord> kept) noexcept {
    for (u32 slot = 0; slot < mesh.tile_capacity(); ++slot) {
        const TileCoord coord = mesh.tile_coord(slot);
        if (mesh.tile_slot(coord) == slot && !listed(kept, coord)) {
            if (Status removed = mesh.remove_tile(coord); !removed) {
                return removed;
            }
        }
    }
    return ok();
}

[[nodiscard]] Expected<NavBakeReport, Error> bake_region(
    Allocator& allocator, const NavBakeSettings& settings, const NavBakeSource& source,
    const Aabb& region, NavMesh& mesh, NavBakeObserver* observer, bool full) noexcept {
    BakeInputs inputs(allocator);
    if (Status prepared = prepare(settings, source, mesh, inputs); !prepared) {
        return make_unexpected(prepared.error());
    }
    Array<TileCoord> coords(allocator);
    if (Status listed_tiles = tiles_overlapping(settings.tile_size, region, coords);
        !listed_tiles) {
        return make_unexpected(listed_tiles.error());
    }
    if (full) {
        if (Status removed = remove_outside(mesh, coords.span()); !removed) {
            return make_unexpected(removed.error());
        }
    }
    return bake_coords(allocator, settings, inputs, coords.span(), mesh, observer);
}

/// The resident tiles under `extent`, appended to `out` without duplicates.
[[nodiscard]] Status append_resident(const NavMesh& mesh, const Aabb& extent,
                                     Array<TileCoord>& out) noexcept {
    Array<TileCoord> coords(mesh.allocator());
    if (Status found = tiles_overlapping(mesh.tile_size(), extent, coords); !found) {
        return found;
    }
    for (const TileCoord& coord : coords.span()) {
        if (mesh.tile_resident(coord) && !listed(out.span(), coord)) {
            if (Status pushed = out.push_back(coord); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

[[nodiscard]] Status tile_span(f32 low, f32 high, f32 size, i32& first, i32& last) noexcept {
    const f32 lo = std::floor(low / size);
    const f32 hi = std::ceil(high / size) - 1.0F;
    if (!std::isfinite(lo) || !std::isfinite(hi) || std::fabs(lo) > kMaxTileIndex ||
        std::fabs(hi) > kMaxTileIndex) {
        return make_unexpected(
            Error{ErrorCode::OutOfRange, "the region lies outside the navigation tile grid"});
    }
    first = static_cast<i32>(lo);
    last = std::max(static_cast<i32>(hi), first);
    return ok();
}

/// Whether tile `coord` is one `tiles_overlapping(tile_size, region)` lists.
[[nodiscard]] bool tile_in_region(f32 tile_size, const Aabb& region, TileCoord coord) noexcept {
    if (region.is_empty() || coord.layer != 0) {
        return false;
    }
    i32 first_x = 0;
    i32 last_x = 0;
    i32 first_z = 0;
    i32 last_z = 0;
    return tile_span(region.min.x, region.max.x, tile_size, first_x, last_x).has_value() &&
           tile_span(region.min.z, region.max.z, tile_size, first_z, last_z).has_value() &&
           coord.x >= first_x && coord.x <= last_x && coord.z >= first_z && coord.z <= last_z;
}

/// Removes the resident tile at `coord`, listing it in `report` as an empty tile with no digest.
[[nodiscard]] Status drop_tile(NavMesh& mesh, TileCoord coord, NavBakeReport& report) noexcept {
    if (!mesh.tile_resident(coord)) {
        return ok();
    }
    if (Status removed = mesh.remove_tile(coord); !removed) {
        return removed;
    }
    ++report.tiles_empty;
    return report.tiles.push_back(NavBakeTile{coord, 0, true, 0});
}

}  // namespace

Status validate_bake_settings(const NavBakeSettings& settings) noexcept {
    if (settings.backend != NavBuildBackend::Engine &&
        settings.backend != NavBuildBackend::Recast) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a bake must name its back end: Engine or Recast"});
    }
    if (!positive(settings.cell_size) || !positive(settings.cell_height) ||
        !positive(settings.tile_size) || !positive(settings.agent_height)) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "cell size, cell height, tile size and agent height must be "
                                     "positive"});
    }
    if (!std::isfinite(settings.agent_radius) || settings.agent_radius < 0.0F ||
        !std::isfinite(settings.step_height) || settings.step_height < 0.0F ||
        !std::isfinite(settings.max_slope_degrees)) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "agent radius and step height must not be negative"});
    }
    const f32 cells = std::ceil(settings.tile_size / settings.cell_size);
    if (cells < 1.0F || cells * cells > kMaxCellsPerTile) {
        return make_unexpected(Error{ErrorCode::OutOfRange,
                                     "the tile size must hold between one cell and four million "
                                     "voxel columns"});
    }
    return ok();
}

NavBuildParams build_params(const NavBakeSettings& settings) noexcept {
    NavBuildParams params;
    params.cell_size = settings.cell_size;
    params.cell_height = settings.cell_height;
    params.agent_radius = settings.agent_radius;
    params.agent_height = settings.agent_height;
    params.agent_max_climb = settings.step_height;
    params.agent_max_slope_degrees = settings.max_slope_degrees;
    params.layers = settings.layers;
    params.tags = settings.tags;
    params.backend = settings.backend;
    return params;
}

AgentProfile agent_profile(const NavBakeSettings& settings, Name name) noexcept {
    AgentProfile profile;
    profile.name = name;
    profile.radius = settings.agent_radius;
    profile.height = settings.agent_height;
    profile.max_climb = settings.step_height;
    profile.max_slope_degrees = settings.max_slope_degrees;
    return profile;
}

Status tiles_overlapping(f32 tile_size, const Aabb& region, Array<TileCoord>& out) noexcept {
    out.clear();
    if (!positive(tile_size)) {
        return make_unexpected(Error{ErrorCode::InvalidArgument, "the tile size must be positive"});
    }
    if (region.is_empty()) {
        return ok();
    }
    i32 first_x = 0;
    i32 last_x = 0;
    i32 first_z = 0;
    i32 last_z = 0;
    if (Status x = tile_span(region.min.x, region.max.x, tile_size, first_x, last_x); !x) {
        return x;
    }
    if (Status z = tile_span(region.min.z, region.max.z, tile_size, first_z, last_z); !z) {
        return z;
    }
    const i64 count = (i64{last_x} - i64{first_x} + 1) * (i64{last_z} - i64{first_z} + 1);
    if (count > kMaxTilesPerRegion) {
        return make_unexpected(
            Error{ErrorCode::OutOfRange, "the region covers more than 65536 navigation tiles"});
    }
    if (Status reserved = out.reserve(static_cast<usize>(count)); !reserved) {
        return reserved;
    }
    for (i32 z = first_z; z <= last_z; ++z) {
        for (i32 x = first_x; x <= last_x; ++x) {
            if (Status pushed = out.push_back(TileCoord{x, z, 0}); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

Aabb surface_region(Span<const NavSurfaceVolume> surfaces) noexcept {
    Aabb region = Aabb::empty();
    for (const NavSurfaceVolume& surface : surfaces) {
        if (!surface.exclude && !surface.bounds.is_empty()) {
            region.grow(surface.bounds.min);
            region.grow(surface.bounds.max);
        }
    }
    return region;
}

Aabb bake_tile_bounds(const NavBakeSettings& settings, const NavSourceGeometry& geometry,
                      TileCoord coord) noexcept {
    return tile_box(settings, vertical_range(geometry), coord);
}

Status assign_triangle_areas(const NavBakeSource& source, Array<AreaType>& out) noexcept {
    const NavSourceGeometry& geometry = source.geometry;
    const usize triangles = geometry.indices.size() / 3;
    out.clear();
    if (Status sized = out.resize(triangles); !sized) {
        return sized;
    }
    for (usize triangle = 0; triangle < triangles; ++triangle) {
        const AreaType base =
            (triangle < geometry.area.size()) ? geometry.area[triangle] : kAreaGround;
        Vec3 centroid;
        if (!triangle_centroid(geometry, triangle, centroid)) {
            return make_unexpected(Error{ErrorCode::InvalidArgument,
                                         "a triangle names a vertex the geometry has not"});
        }
        out[triangle] = winning_area(source.areas, centroid, base);
    }
    return ok();
}

NavAreaCosts area_costs(Span<const NavAreaVolume> areas) noexcept {
    NavAreaCosts costs = NavAreaCosts::uniform();
    u64 owner[kAreaCount] = {};
    bool owned[kAreaCount] = {};
    for (const NavAreaVolume& volume : areas) {
        const u32 area = volume.area & (kAreaCount - 1);
        if (!owned[area] || volume.node >= owner[area]) {
            costs.multiplier[area] = volume.cost;
            owner[area] = volume.node;
            owned[area] = true;
        }
    }
    return costs;
}

Expected<NavBakeReport, Error> bake_tiles(Allocator& allocator, const NavBakeSettings& settings,
                                          const NavBakeSource& source, const Aabb& region,
                                          NavMesh& mesh, NavBakeObserver* observer) noexcept {
    return bake_region(allocator, settings, source, region, mesh, observer, true);
}

Expected<NavBakeReport, Error> rebake_tiles(Allocator& allocator, const NavBakeSettings& settings,
                                            const NavBakeSource& source, const Aabb& dirty,
                                            NavMesh& mesh, NavBakeObserver* observer) noexcept {
    return bake_region(allocator, settings, source, dirty, mesh, observer, false);
}

Expected<NavBakeReport, Error> rebake_surface_tiles(Allocator& allocator,
                                                    const NavBakeSettings& settings,
                                                    const NavBakeSource& source, const Aabb& dirty,
                                                    NavMesh& mesh) noexcept {
    BakeInputs inputs(allocator);
    if (Status prepared = prepare(settings, source, mesh, inputs); !prepared) {
        return make_unexpected(prepared.error());
    }
    Array<TileCoord> coords(allocator);
    if (Status listed_tiles = tiles_overlapping(settings.tile_size, dirty, coords); !listed_tiles) {
        return make_unexpected(listed_tiles.error());
    }
    const Aabb region = surface_region(source.surfaces);
    NavBakeReport report(allocator);
    report.totals.backend = settings.backend;
    for (const TileCoord& coord : coords.span()) {
        if (!tile_in_region(settings.tile_size, region, coord)) {
            if (Status dropped = drop_tile(mesh, coord, report); !dropped) {
                return make_unexpected(dropped.error());
            }
            continue;
        }
        NavBakeTile baked;
        if (Status done = bake_one(allocator, settings, inputs, coord, mesh, report, baked);
            !done) {
            return make_unexpected(done.error());
        }
        if (Status pushed = report.tiles.push_back(baked); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    return report;
}

Expected<NavObstacleChange, Error> place_obstacle(NavMesh& mesh,
                                                  const NavObstacleShape& shape) noexcept {
    NavObstacleChange change(mesh.allocator());
    Expected<ObstacleId, Error> added = mesh.add_obstacle(shape);
    if (!added) {
        return make_unexpected(added.error());
    }
    change.id = *added;
    if (Status found = append_resident(mesh, shape.extent(), change.affected); !found) {
        return make_unexpected(found.error());
    }
    return change;
}

Expected<NavObstacleChange, Error> move_obstacle(NavMesh& mesh, ObstacleId id,
                                                 const NavObstacleShape& next) noexcept {
    const NavObstacleShape* current = mesh.obstacle(id);
    if (current == nullptr) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such navigation obstacle"});
    }
    const NavObstacleShape previous = *current;
    Expected<NavObstacleChange, Error> cleared = clear_obstacle(mesh, id);
    if (!cleared) {
        return cleared;
    }
    Expected<NavObstacleChange, Error> placed = place_obstacle(mesh, next);
    if (!placed) {
        // Put the old footprint back, so a failed move leaves the mesh as it was.
        (void)mesh.add_obstacle(previous);
        return placed;
    }
    for (const TileCoord& coord : cleared->affected.span()) {
        if (!listed(placed->affected.span(), coord)) {
            if (Status pushed = placed->affected.push_back(coord); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
    }
    return placed;
}

Expected<NavObstacleChange, Error> clear_obstacle(NavMesh& mesh, ObstacleId id) noexcept {
    const NavObstacleShape* current = mesh.obstacle(id);
    if (current == nullptr) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such navigation obstacle"});
    }
    NavObstacleChange change(mesh.allocator());
    change.id = id;
    if (Status found = append_resident(mesh, current->extent(), change.affected); !found) {
        return make_unexpected(found.error());
    }
    if (Status removed = mesh.remove_obstacle(id); !removed) {
        return make_unexpected(removed.error());
    }
    return change;
}

}  // namespace cy::navigation
