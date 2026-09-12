// Meshing a tile: the grid, the level stitch, the triangles a hole does not get, and the key that
// makes the result cacheable. M10 tasks 2.1 and 2.2.

#include <cy/terrain/meshing.h>

#include <bit>
#include <cmath>

namespace cy::terrain {
namespace {

/// How many fine samples a coarser neighbour's spacing covers, in this mesh's own index space.
[[nodiscard]] u32 stitch_step(const TileLayout& layout, u8 own_level, u8 neighbour_level) noexcept {
    if (neighbour_level <= own_level) {
        return 1;
    }
    u32 step = 1;
    for (u8 level = own_level; level < neighbour_level; ++level) {
        step *= layout.level_ratio;
    }
    return step;
}

/// The heights of one tile, at the mesh's own stride, before the stitch.
struct MeshGrid {
    Array<f32> heights;
    u32 edge = 0;    ///< samples per edge
    u32 stride = 1;  ///< fine samples between mesh samples

    explicit MeshGrid(Allocator& allocator) noexcept : heights(allocator) {}

    [[nodiscard]] f32& at(u32 i, u32 j) noexcept { return heights[(j * edge) + i]; }
    [[nodiscard]] f32 at(u32 i, u32 j) const noexcept { return heights[(j * edge) + i]; }
};

[[nodiscard]] Expected<MeshGrid, Error> build_grid(Allocator& allocator, const TileCoord& coord,
                                                   const HeightfieldSource& heights,
                                                   const TerrainDeltaStore* visual,
                                                   const MeshOptions& options) noexcept {
    const u32 stride = options.virtual_geometry ? 1U : options.fallback_decimation;
    if (stride == 0 || kTileQuads % stride != 0) {
        return fail(ErrorCode::InvalidArgument,
                    "terrain: the fallback decimation must divide the tile's quad count");
    }
    MeshGrid grid(allocator);
    grid.stride = stride;
    grid.edge = (kTileQuads / stride) + 1;
    if (Status sized = grid.heights.resize(static_cast<usize>(grid.edge) * grid.edge); !sized) {
        return make_unexpected(sized.error());
    }
    for (u32 j = 0; j < grid.edge; ++j) {
        for (u32 i = 0; i < grid.edge; ++i) {
            const u32 fine_i = i * stride;
            const u32 fine_j = j * stride;
            f32 height = heights.sample_height(coord, fine_i, fine_j);
            if (options.include_visual && visual != nullptr) {
                // Ruts and footprints reach the mesh and reach nothing else. deform.h's header
                // note.
                height += visual->visual_delta(coord, fine_i, fine_j);
            }
            grid.at(i, j) = height;
        }
    }
    return grid;
}

/// Move one edge's in-between samples onto the segment the coarser neighbour will draw.
[[nodiscard]] u32 stitch(MeshGrid& grid, TileEdge edge, u32 step) noexcept {
    if (step <= 1) {
        return 0;
    }
    const u32 last = grid.edge - 1;
    u32 moved = 0;
    for (u32 index = 1; index < last; ++index) {
        if (index % step == 0) {
            continue;  // A shared sample: the coarse tile has this one too.
        }
        const u32 low = (index / step) * step;
        const u32 high = low + step;
        if (high > last) {
            continue;
        }
        f32* target = nullptr;
        f32 low_height = 0.0F;
        f32 high_height = 0.0F;
        switch (edge) {
            case TileEdge::NegativeX:
                low_height = grid.at(0, low);
                high_height = grid.at(0, high);
                target = &grid.at(0, index);
                break;
            case TileEdge::PositiveX:
                low_height = grid.at(last, low);
                high_height = grid.at(last, high);
                target = &grid.at(last, index);
                break;
            case TileEdge::NegativeZ:
                low_height = grid.at(low, 0);
                high_height = grid.at(high, 0);
                target = &grid.at(index, 0);
                break;
            case TileEdge::PositiveZ:
                low_height = grid.at(low, last);
                high_height = grid.at(high, last);
                target = &grid.at(index, last);
                break;
            case TileEdge::kCount:
                return moved;
        }
        *target = stitch_edge(low_height, high_height, index - low, step);
        ++moved;
    }
    return moved;
}

[[nodiscard]] Vec3 grid_normal(const MeshGrid& grid, f32 spacing, u32 i, u32 j) noexcept {
    const u32 last = grid.edge - 1;
    const u32 left = (i == 0) ? 0 : i - 1;
    const u32 right = (i == last) ? last : i + 1;
    const u32 down = (j == 0) ? 0 : j - 1;
    const u32 up = (j == last) ? last : j + 1;
    const f32 dx =
        (grid.at(right, j) - grid.at(left, j)) / (static_cast<f32>(right - left) * spacing);
    const f32 dz = (grid.at(i, up) - grid.at(i, down)) / (static_cast<f32>(up - down) * spacing);
    Vec3 normal{-dx, 1.0F, -dz};
    const f32 length = std::sqrt((normal.x * normal.x) + 1.0F + (normal.z * normal.z));
    return Vec3{normal.x / length, normal.y / length, normal.z / length};
}

[[nodiscard]] u64 mesh_key(const TileLayout& layout, const TileCoord& coord,
                           const MeshOptions& options, const TerrainDeltaStore* deltas,
                           const MeshGrid& grid) noexcept {
    u64 key = hash_integer(tile_id_of(layout, coord).value, kTerrainHashSeed);
    for (const u8 edge : options.neighbour_level) {
        key = hash_combine(key, edge);
    }
    key = hash_combine(key, options.include_visual ? 1U : 0U);
    key = hash_combine(key, options.virtual_geometry ? 1U : 0U);
    key = hash_combine(key, grid.stride);
    key = hash_combine(key, (deltas != nullptr && deltas->has_delta(coord)) ? 1U : 0U);
    // The grid itself, because the delta's VALUES are part of what the mesh is a function of and a
    // key that recorded only "there is a delta" would serve a stale mesh after a second crater in
    // the same tile.
    for (const f32 height : grid.heights.span()) {
        key = hash_combine(key, static_cast<u64>(std::bit_cast<u32>(height)));
    }
    return key;
}

/// The vertices, normals and UVs of one meshed tile. Positions are LOCAL to the tile's minimum
/// corner, for the precision reason meshing.h gives.
[[nodiscard]] Status emit_vertices(TerrainMesh& mesh, const MeshGrid& grid, f32 spacing) noexcept {
    for (u32 j = 0; j < grid.edge; ++j) {
        for (u32 i = 0; i < grid.edge; ++i) {
            const f32 local_x = static_cast<f32>(i) * spacing;
            const f32 local_z = static_cast<f32>(j) * spacing;
            if (Status pushed = mesh.positions.push_back(Vec3{local_x, grid.at(i, j), local_z});
                !pushed) {
                return pushed;
            }
            if (Status pushed = mesh.normals.push_back(grid_normal(grid, spacing, i, j)); !pushed) {
                return pushed;
            }
            const f32 u = static_cast<f32>(i) / static_cast<f32>(grid.edge - 1);
            const f32 v = static_cast<f32>(j) / static_cast<f32>(grid.edge - 1);
            if (Status pushed = mesh.uvs.push_back(Vec2{u, v}); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

/// One quad's two triangles, and the material they carry.
[[nodiscard]] Status emit_quad(TerrainMesh& mesh, const TerrainTile& tile, u32 edge_samples, u32 i,
                               u32 j, u32 fine_i, u32 fine_j) noexcept {
    const u32 base = (j * edge_samples) + i;
    const u32 corners[6] = {base,     base + edge_samples, base + 1,
                            base + 1, base + edge_samples, base + edge_samples + 1};
    for (const u32 corner : corners) {
        if (Status pushed = mesh.indices.push_back(corner); !pushed) {
            return pushed;
        }
    }
    const u32 material = tile.texel(fine_i, fine_j).dominant();
    for (u32 triangle = 0; triangle < 2; ++triangle) {
        if (Status pushed = mesh.triangle_materials.push_back(material); !pushed) {
            return pushed;
        }
    }
    return ok();
}

/// Every quad of the tile that is not a hole. A hole is a hole in the geometry too: the quad is not
/// emitted, so the surface has an opening rather than a face nobody can see through.
[[nodiscard]] Status emit_triangles(TerrainMesh& mesh, const TerrainTile& tile,
                                    const TerrainDeltaStore* visual, const TileCoord& coord,
                                    const MeshGrid& grid, MeshReport& report) noexcept {
    for (u32 j = 0; j + 1 < grid.edge; ++j) {
        for (u32 i = 0; i + 1 < grid.edge; ++i) {
            const u32 fine_i = i * grid.stride;
            const u32 fine_j = j * grid.stride;
            const bool carved = (visual != nullptr) && visual->hole_delta(coord, fine_i, fine_j);
            if (tile.hole(fine_i, fine_j) || carved) {
                ++report.hole_quads;
                continue;
            }
            if (Status emitted = emit_quad(mesh, tile, grid.edge, i, j, fine_i, fine_j); !emitted) {
                return emitted;
            }
        }
    }
    return ok();
}

}  // namespace

f32 stitch_edge(f32 low, f32 high, u32 index, u32 step) noexcept {
    if (step == 0) {
        return low;
    }
    const f32 t = static_cast<f32>(index) / static_cast<f32>(step);
    return low + ((high - low) * t);
}

Expected<TerrainMesh, Error> mesh_tile(Allocator& allocator, const TerrainStore& store,
                                       const HeightfieldSource& heights,
                                       const TerrainDeltaStore* visual, const TileCoord& coord,
                                       const MeshOptions& options, MeshReport& report) noexcept {
    const TerrainTile* tile = store.find(coord);
    if (tile == nullptr) {
        return fail(ErrorCode::NotFound, "terrain: that tile is not resident and cannot be meshed");
    }
    const TileLayout& layout = store.layout();

    Expected<MeshGrid, Error> built = build_grid(allocator, coord, heights, visual, options);
    if (!built) {
        return make_unexpected(built.error());
    }
    MeshGrid grid = std::move(built.value());

    report = MeshReport{};
    for (u32 edge = 0; edge < kTileEdgeCount; ++edge) {
        report.stitched_vertices +=
            stitch(grid, static_cast<TileEdge>(edge),
                   stitch_step(layout, coord.level, options.neighbour_level[edge]));
    }

    TerrainMesh mesh(allocator);
    mesh.coord = coord;
    mesh.bounds = tile_bounds(layout, coord);
    const f32 spacing = layout.sample_metres(coord.level) * static_cast<f32>(grid.stride);
    if (Status emitted = emit_vertices(mesh, grid, spacing); !emitted) {
        return make_unexpected(emitted.error());
    }
    if (Status emitted = emit_triangles(mesh, *tile, visual, coord, grid, report); !emitted) {
        return make_unexpected(emitted.error());
    }

    mesh.key = mesh_key(layout, coord, options, visual, grid);
    report.vertices = static_cast<u32>(mesh.positions.size());
    report.triangles = static_cast<u32>(mesh.triangle_count());
    report.fallback = !options.virtual_geometry;
    report.detail = 1.0F / static_cast<f32>(grid.stride);
    if (report.fallback) {
        report.limitation =
            "terrain: virtual geometry is unavailable; tiles mesh through the conventional path at "
            "reduced sample density";
    }
    return mesh;
}

}  // namespace cy::terrain
