// SPDX-License-Identifier: MIT
// An author's terrain region: evaluated from the modifier stack, stitched, meshed and collided.
// Issue #29, "Terrain (finish)".

#include <cy/terrain/collision.h>
#include <cy/terrain/deform.h>
#include <cy/terrain/meshing.h>
#include <cy/terrain/region.h>
#include <cy/terrain/surface.h>

#include <cmath>

namespace cy::terrain {
namespace {

[[nodiscard]] MeshOptions same_level_mesh() noexcept {
    MeshOptions options;
    for (u8& level : options.neighbour_level) {
        level = 0;
    }
    options.include_visual = false;
    return options;
}

/// Copy one tile's heights, texels and holes into the stitched lattice. A shared boundary sample
/// is written by both tiles with the same value, so the order does not matter.
void stitch(const TerrainTile& tile, u32 tile_x, u32 tile_z, RegionSnapshot& out) noexcept {
    const u32 edge = out.edge;
    const u32 quads = edge - 1;
    for (u32 j = 0; j < kTileVerts; ++j) {
        for (u32 i = 0; i < kTileVerts; ++i) {
            const u32 x = (tile_x * kTileQuads) + i;
            const u32 z = (tile_z * kTileQuads) + j;
            out.heights[(static_cast<usize>(z) * edge) + x] = tile.stored(i, j);
        }
    }
    for (u32 j = 0; j < kTileTexels; ++j) {
        for (u32 i = 0; i < kTileTexels; ++i) {
            const usize at =
                (static_cast<usize>((tile_z * kTileQuads) + j) * quads) + (tile_x * kTileQuads) + i;
            out.texels[at] = tile.texel(i, j);
            out.holes[at] = tile.hole(i, j) ? 1U : 0U;
        }
    }
}

/// Mesh and collide one resident tile, adding what each left open to the snapshot's counts.
[[nodiscard]] Status measure(Allocator& allocator, const TerrainStore& store,
                             const HeightfieldSource& heights, const TileCoord& coord,
                             RegionSnapshot& out) noexcept {
    MeshReport report;
    Expected<TerrainMesh, Error> mesh =
        mesh_tile(allocator, store, heights, nullptr, coord, same_level_mesh(), report);
    if (!mesh) {
        return make_unexpected(mesh.error());
    }
    out.rendered_triangles += report.triangles;
    out.rendered_hole_quads += report.hole_quads;
    // Tile-local positions become region positions; the tile's own index list is kept.
    const TerrainMesh& tile = mesh.value();
    const auto base = static_cast<u32>(out.positions.size());
    const Vec3 origin{static_cast<f32>(tile.bounds.min_x), 0.0F,
                      static_cast<f32>(tile.bounds.min_z)};
    for (usize vertex = 0; vertex < tile.positions.size(); ++vertex) {
        if (Status pushed = out.positions.push_back(tile.positions[vertex] + origin); !pushed) {
            return pushed;
        }
        const Vec3 normal = (vertex < tile.normals.size()) ? tile.normals[vertex] : Vec3{0, 1, 0};
        if (Status pushed = out.normals.push_back(normal); !pushed) {
            return pushed;
        }
    }
    for (const u32 index : tile.indices.span()) {
        if (Status pushed = out.indices.push_back(base + index); !pushed) {
            return pushed;
        }
    }
    Expected<CollisionTile, Error> collision =
        build_collision(allocator, store, heights, coord, CollisionConfig{});
    if (!collision) {
        return make_unexpected(collision.error());
    }
    out.collision_holes += collision.value().holes;
    return ok();
}

}  // namespace

TileLayout region_layout(const RegionDescription& region) noexcept {
    TileLayout layout;
    layout.scheme = CoordinateScheme::PlanarGrid;
    layout.terrain = region.terrain;
    layout.tile_metres = region.extent / static_cast<f32>((region.tiles == 0) ? 1 : region.tiles);
    layout.levels = 1;
    layout.level_ratio = 2;
    layout.height_min = -512.0F;
    layout.height_max = 1536.0F;
    return layout;
}

Status validate_region(const RegionDescription& region) noexcept {
    if (region.tiles == 0 || region.tiles > kMaxRegionTiles) {
        return fail(ErrorCode::InvalidArgument,
                    "terrain: a region is between one and sixteen tiles along an edge");
    }
    if (!std::isfinite(region.extent) || !(region.extent > 0.0F) || region.extent > 65536.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "terrain: a region needs a finite extent between zero and 65536 metres");
    }
    return ok();
}

Expected<RegionSnapshot, Error> evaluate_region(Allocator& allocator, const ModifierStack& stack,
                                                const RegionDescription& region) noexcept {
    if (Status valid = validate_region(region); !valid) {
        return make_unexpected(valid.error());
    }
    const TileLayout layout = region_layout(region);
    TerrainStore store(allocator, layout);
    const auto last = static_cast<i32>(region.tiles) - 1;
    if (Status flattened = stack.flatten(store, 0, 0, last, last, 0); !flattened) {
        return make_unexpected(flattened.error());
    }

    RegionSnapshot out(allocator);
    out.edge = (region.tiles * kTileQuads) + 1;
    const usize quads = static_cast<usize>(out.edge - 1) * (out.edge - 1);
    if (Status sized = out.heights.resize(static_cast<usize>(out.edge) * out.edge); !sized) {
        return make_unexpected(sized.error());
    }
    if (Status sized = out.texels.resize(quads); !sized) {
        return make_unexpected(sized.error());
    }
    if (Status sized = out.holes.resize(quads); !sized) {
        return make_unexpected(sized.error());
    }

    const HeightfieldSource heights(store, nullptr);
    for (u32 z = 0; z < region.tiles; ++z) {
        for (u32 x = 0; x < region.tiles; ++x) {
            const TileCoord coord{static_cast<i32>(x), static_cast<i32>(z), 0};
            const TerrainTile* tile = store.find(coord);
            if (tile == nullptr) {
                return make_unexpected(
                    Error{ErrorCode::Internal, "terrain: a flattened region tile is missing"});
            }
            stitch(*tile, x, z, out);
            if (Status measured = measure(allocator, store, heights, coord, out); !measured) {
                return make_unexpected(measured.error());
            }
        }
    }
    return out;
}

Expected<f32, Error> region_height_at(Allocator& allocator, const ModifierStack& stack,
                                      const RegionDescription& region, f64 x, f64 z) noexcept {
    if (Status valid = validate_region(region); !valid) {
        return make_unexpected(valid.error());
    }
    const TileLayout layout = region_layout(region);
    const f64 spacing = static_cast<f64>(layout.sample_metres(0));
    const f64 limit = static_cast<f64>(region.tiles * kTileQuads);
    const f64 column = std::round(x / spacing);
    const f64 row = std::round(z / spacing);
    const auto sample_x =
        static_cast<u32>((column < 0.0) ? 0.0 : ((column > limit) ? limit : column));
    const auto sample_z = static_cast<u32>((row < 0.0) ? 0.0 : ((row > limit) ? limit : row));
    // The last lattice line belongs to the previous tile's far edge, which holds the same value.
    const u32 tile_x =
        (sample_x == region.tiles * kTileQuads) ? region.tiles - 1 : sample_x / kTileQuads;
    const u32 tile_z =
        (sample_z == region.tiles * kTileQuads) ? region.tiles - 1 : sample_z / kTileQuads;
    Expected<TerrainTile, Error> tile =
        stack.evaluate(allocator, TileCoord{static_cast<i32>(tile_x), static_cast<i32>(tile_z), 0});
    if (!tile) {
        return make_unexpected(tile.error());
    }
    return dequantise_height(layout, tile.value().stored(sample_x - (tile_x * kTileQuads),
                                                         sample_z - (tile_z * kTileQuads)));
}

}  // namespace cy::terrain
