// The Recast back end of `cy::navigation::build_tile()`. THE ONLY TRANSLATION UNIT IN THIS ENGINE
// THAT NAMES AN `rc` SYMBOL, and CMake compiles it only when `CY_NAVIGATION` is on.
//
// ================================================================================================
// WHAT THIS FILE IS AND IS NOT
// ================================================================================================
//
// It is an adapter, and a short one: Recast's pipeline runs, and its `rcPolyMesh` is copied into
// this engine's `NavTileData`. It is NOT where a navigation decision is made — the filters are
// build.cpp's, the tiling is `NavMesh`'s, and the adjacency across a tile border is
// `NavMesh::add_tile`'s, because Recast's own adjacency is per polygon-mesh and this engine
// publishes tiles independently.
//
// Detour is not linked. deps/manifest.toml's `source_subdir = "Recast"` is what makes that a build
// fact; navmesh.h carries the reasoning.
//
// ================================================================================================
// THE AREA CONVENTIONS ARE OPPOSITE, AND THAT IS THE ONE TRAP IN THIS FILE
// ================================================================================================
//
// Recast: `RC_NULL_AREA` is 0 and `RC_WALKABLE_AREA` is 63.
// This engine: `kAreaGround` is 0 and `kAreaNull` is 63 — see navmesh.h, which sized `kAreaCount`
// against Recast's own area byte and then chose the readable default for zero.
//
// So the mapping is a swap of 0 and 63 and an identity on everything between, applied in both
// directions by `swap_area()`. Getting it wrong does not fail to build and does not crash: it
// produces a mesh in which every walkable polygon is marked unwalkable, which looks exactly like a
// mesh that failed to generate.

#include <cy/core/base/assert.h>
#include <cy/navigation/build.h>

#include <Recast.h>

#include <cmath>
#include <cstddef>

#include "recast_bridge.h"

namespace cy::navigation::detail {
namespace {

/// Recast's area convention and this engine's differ by exactly this swap. See the header.
[[nodiscard]] u8 swap_area(u8 area) noexcept {
    if (area == 0) {
        return 63;
    }
    if (area == 63) {
        return 0;
    }
    return area;
}

/// Owns Recast's allocations, so an early return releases them. Recast has no RAII of its own and
/// the engine builds with -fno-exceptions, but a `return` out of a validation branch is exactly the
/// leak this closes.
class RecastBuild {
public:
    RecastBuild() noexcept = default;
    ~RecastBuild() {
        rcFreePolyMesh(mesh);
        rcFreeContourSet(contours);
        rcFreeCompactHeightfield(compact);
        rcFreeHeightField(heightfield);
    }

    RecastBuild(const RecastBuild&) = delete;
    RecastBuild& operator=(const RecastBuild&) = delete;
    RecastBuild(RecastBuild&&) = delete;
    RecastBuild& operator=(RecastBuild&&) = delete;

    rcContext context{false};  ///< timers off: a cook must not read a clock
    rcHeightfield* heightfield = nullptr;
    rcCompactHeightfield* compact = nullptr;
    rcContourSet* contours = nullptr;
    rcPolyMesh* mesh = nullptr;
};

[[nodiscard]] int to_voxels(f32 world, f32 cell) noexcept {
    return static_cast<int>(std::ceil(world / cell));
}

/// `NavBuildParams` in Recast's own units. Every conversion from metres to voxels is here and
/// nowhere else.
[[nodiscard]] rcConfig to_config(const NavBuildParams& params, const Aabb& bounds) noexcept {
    rcConfig config = {};
    config.cs = params.cell_size;
    config.ch = params.cell_height;
    config.bmin[0] = bounds.min.x;
    config.bmin[1] = bounds.min.y;
    config.bmin[2] = bounds.min.z;
    config.bmax[0] = bounds.max.x;
    config.bmax[1] = bounds.max.y;
    config.bmax[2] = bounds.max.z;
    // The slope limit was applied in build.cpp, which marked every steep triangle `kAreaNull`, so
    // Recast is told 90 degrees and never re-decides it. One place decides walkability.
    config.walkableSlopeAngle = 90.0f;
    config.walkableHeight = to_voxels(params.agent_height, params.cell_height);
    config.walkableClimb = to_voxels(params.agent_max_climb, params.cell_height);
    config.walkableRadius = to_voxels(params.agent_radius, params.cell_size);
    config.maxEdgeLen = to_voxels(params.edge_max_length, params.cell_size);
    config.maxSimplificationError = params.edge_max_error;
    config.minRegionArea = static_cast<int>(params.region_min_size * params.region_min_size);
    config.mergeRegionArea = static_cast<int>(params.region_merge_size * params.region_merge_size);
    config.maxVertsPerPoly = static_cast<int>(kMaxPolyVertices);
    config.detailSampleDist = params.detail_sample_distance * params.cell_size;
    config.detailSampleMaxError = params.detail_sample_max_error * params.cell_height;
    rcCalcGridSize(config.bmin, config.bmax, config.cs, &config.width, &config.height);
    return config;
}

/// The filtered triangles, in Recast's index and area conventions.
[[nodiscard]] Status rasterise(RecastBuild& build, const rcConfig& config, Allocator& allocator,
                               const FilteredGeometry& geometry) noexcept {
    const auto triangle_count = static_cast<int>(geometry.areas.size());
    if (triangle_count == 0) {
        return ok();
    }
    Array<int> indices(allocator);
    Array<unsigned char> areas(allocator);
    if (Status sized = indices.resize(geometry.indices.size()); !sized) {
        return sized;
    }
    if (Status sized = areas.resize(geometry.areas.size()); !sized) {
        return sized;
    }
    for (usize i = 0; i < geometry.indices.size(); ++i) {
        indices[i] = static_cast<int>(geometry.indices[i]);
    }
    for (usize i = 0; i < geometry.areas.size(); ++i) {
        areas[i] = swap_area(geometry.areas[i]);
    }
    if (!rcRasterizeTriangles(
            &build.context, reinterpret_cast<const float*>(geometry.vertices.data()),
            static_cast<int>(geometry.vertices.size()), indices.span().data(), areas.span().data(),
            triangle_count, *build.heightfield, config.walkableClimb)) {
        return make_unexpected(
            Error{ErrorCode::Internal, "Recast could not rasterise the source triangles"});
    }
    return ok();
}

/// Recast's pipeline, from an empty heightfield to a polygon mesh.
[[nodiscard]] Status run_pipeline(RecastBuild& build, const rcConfig& config, Allocator& allocator,
                                  const FilteredGeometry& geometry,
                                  NavBuildReport& report) noexcept {
    build.heightfield = rcAllocHeightfield();
    if (build.heightfield == nullptr ||
        !rcCreateHeightfield(&build.context, *build.heightfield, config.width, config.height,
                             config.bmin, config.bmax, config.cs, config.ch)) {
        return make_unexpected(
            Error{ErrorCode::OutOfMemory, "Recast could not allocate the heightfield"});
    }
    if (Status rasterised = rasterise(build, config, allocator, geometry); !rasterised) {
        return rasterised;
    }

    rcFilterLowHangingWalkableObstacles(&build.context, config.walkableClimb, *build.heightfield);
    rcFilterLedgeSpans(&build.context, config.walkableHeight, config.walkableClimb,
                       *build.heightfield);
    rcFilterWalkableLowHeightSpans(&build.context, config.walkableHeight, *build.heightfield);

    build.compact = rcAllocCompactHeightfield();
    if (build.compact == nullptr ||
        !rcBuildCompactHeightfield(&build.context, config.walkableHeight, config.walkableClimb,
                                   *build.heightfield, *build.compact)) {
        return make_unexpected(
            Error{ErrorCode::Internal, "Recast could not build the compact heightfield"});
    }
    report.spans = static_cast<u32>(build.compact->spanCount);

    // `navigation`: "the walkable surface SHALL be shrunk by that distance from obstacles".
    if (config.walkableRadius > 0 &&
        !rcErodeWalkableArea(&build.context, config.walkableRadius, *build.compact)) {
        return make_unexpected(
            Error{ErrorCode::Internal, "Recast could not erode by the agent radius"});
    }
    if (!rcBuildDistanceField(&build.context, *build.compact) ||
        !rcBuildRegions(&build.context, *build.compact, config.borderSize, config.minRegionArea,
                        config.mergeRegionArea)) {
        return make_unexpected(Error{ErrorCode::Internal, "Recast could not build the regions"});
    }

    build.contours = rcAllocContourSet();
    if (build.contours == nullptr ||
        !rcBuildContours(&build.context, *build.compact, config.maxSimplificationError,
                         config.maxEdgeLen, *build.contours)) {
        return make_unexpected(Error{ErrorCode::Internal, "Recast could not trace the contours"});
    }

    build.mesh = rcAllocPolyMesh();
    if (build.mesh == nullptr ||
        !rcBuildPolyMesh(&build.context, *build.contours, config.maxVertsPerPoly, *build.mesh)) {
        return make_unexpected(
            Error{ErrorCode::Internal, "Recast could not build the polygon mesh"});
    }
    return ok();
}

/// `rcPolyMesh` into `NavTileData`.
///
/// Recast's vertices are voxel coordinates relative to the mesh's own bmin. Its polygon rows are
/// `2 * nvp` entries: `nvp` vertex indices terminated by `RC_MESH_NULL_IDX`, then `nvp` neighbours,
/// which are DISCARDED — `NavMesh::add_tile` recomputes adjacency, because it also has to find the
/// edges that cross into a tile Recast never saw.
[[nodiscard]] Status convert_mesh(const rcPolyMesh& mesh, NavTileData& tile) noexcept {
    if (Status sized = tile.vertices().reserve(static_cast<usize>(mesh.nverts)); !sized) {
        return sized;
    }
    for (int vertex = 0; vertex < mesh.nverts; ++vertex) {
        const unsigned short* source = &mesh.verts[static_cast<ptrdiff_t>(vertex) * 3];
        const Status pushed =
            tile.vertices().push_back(Vec3{mesh.bmin[0] + (static_cast<f32>(source[0]) * mesh.cs),
                                           mesh.bmin[1] + (static_cast<f32>(source[1]) * mesh.ch),
                                           mesh.bmin[2] + (static_cast<f32>(source[2]) * mesh.cs)});
        if (!pushed) {
            return pushed;
        }
    }

    const auto max_corners = static_cast<u32>(mesh.nvp);
    for (int poly = 0; poly < mesh.npolys; ++poly) {
        const unsigned short* row =
            &mesh.polys[static_cast<ptrdiff_t>(poly) * 2 * static_cast<ptrdiff_t>(mesh.nvp)];
        u32 corner_count = 0;
        while (corner_count < max_corners && row[corner_count] != RC_MESH_NULL_IDX) {
            ++corner_count;
        }
        const u8 area = swap_area(mesh.areas[poly]);
        if (corner_count < 3 || area == kAreaNull) {
            continue;
        }
        NavPoly out;
        out.first_corner = static_cast<u32>(tile.corners().size());
        out.corner_count = static_cast<u8>(corner_count);
        out.area = area;
        out.cost = 1.0f;
        if (Status pushed = tile.polys().push_back(out); !pushed) {
            return pushed;
        }
        for (u32 corner = 0; corner < corner_count; ++corner) {
            if (Status pushed = tile.corners().push_back(static_cast<u32>(row[corner])); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

}  // namespace

Expected<NavTileData, Error> build_tile_recast(Allocator& allocator, const NavBuildParams& params,
                                               const FilteredGeometry& geometry, TileCoord coord,
                                               const Aabb& bounds,
                                               NavBuildReport& report) noexcept {
    RecastBuild build;
    const rcConfig config = to_config(params, bounds);
    if (Status ran = run_pipeline(build, config, allocator, geometry, report); !ran) {
        return make_unexpected(ran.error());
    }

    NavTileData tile(allocator);
    tile.coord = coord;
    if (Status converted = convert_mesh(*build.mesh, tile); !converted) {
        return make_unexpected(converted.error());
    }
    report.polys = static_cast<u32>(tile.polys().size());
    report.vertices = static_cast<u32>(tile.vertices().size());
    report.backend = NavBuildBackend::Recast;
    tile.finalise();
    return tile;
}

}  // namespace cy::navigation::detail
