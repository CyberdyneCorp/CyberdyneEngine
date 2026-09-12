#pragma once
// Terrain as a GEOMETRY SOURCE: a tile becomes a mesh, watertight against its neighbours and
// against the levels around it, deterministic and cacheable. M10 tasks 2.1 and 2.2.
//
// `terrain` — "Terrain is a geometry source": rendering "SHALL be produced by MESHING TILES INTO
// VIRTUAL GEOMETRY CLUSTERS that enter the GPU scene like any other geometry", and "There SHALL NOT
// be a terrain-specific renderer, a terrain-specific level-of-detail morphing scheme, or a
// terrain-specific shadow path."
//
// ================================================================================================
// WHAT THIS FILE PRODUCES, AND WHERE THE CLUSTERS ARE ACTUALLY BUILT
// ================================================================================================
//
// This file produces a `TerrainMesh` — positions, normals, UVs and indices — and stops there. The
// conversion into virtual geometry clusters is `cy::terrain-cook`'s (`cook.h`), which calls
// `rendering::vg::build_geometry()` with no terrain-specific anything, and that split is the point:
//
//   * the RUNTIME target does not link the renderer, so a dedicated server meshing terrain for
//     collision pays for none of it, and
//   * the cluster build is demonstrably the ENGINE'S build rather than a terrain copy of it,
//     because the only thing terrain hands it is the same `SourceMesh` a static mesh hands it.
//
// ================================================================================================
// WATERTIGHTNESS HAS TWO CASES AND THIS FILE HANDLES THEM SEPARATELY
// ================================================================================================
//
// "Meshing SHALL be deterministic and cacheable, and SHALL produce watertight boundaries between
// ADJACENT TILES and BETWEEN LEVELS, so no cracks appear at tile or detail transitions."
//
//   * BETWEEN ADJACENT TILES AT ONE LEVEL — free, and deliberately so. A tile owns 64 quads and 65
//     sample lines, so its last line IS its neighbour's first line; `sample_position()` derives
//     both from one absolute lattice index, and heights are quantised against the LAYOUT rather
//     than the tile (tile.h). Two neighbours therefore emit bit-identical boundary vertices with no
//     stitching rule at all. A rule is what you need when the representation does not already
//     agree.
//
//   * BETWEEN LEVELS — a rule, and it is `stitch_edge()`. A coarse tile's samples along a shared
//     edge are a DECIMATION of the fine tile's (see `derive_coarse()`), so the fine tile's
//     in-between vertices must be moved onto the straight segment the coarse tile will draw. The
//     fine tile yields; the coarse tile is never touched. Yielding in the other direction would
//     make a tile's geometry depend on tiles it cannot see, and a tile whose mesh depends on its
//     neighbours' neighbours is not cacheable.
//
// The cache key is `TerrainMesh::key`, and it covers everything the mesh is a function of: the
// layout, the tile, the neighbour levels that drove the stitch, and the delta over the tile. A key
// that omitted the neighbour levels would serve a cracked mesh out of a cache after a level change.

#include <cy/core/base/expected.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/terrain/deform.h>
#include <cy/terrain/surface.h>
#include <cy/terrain/tile.h>

namespace cy::terrain {

/// The four edges of a tile, in the order `MeshOptions::neighbour_level` lists them.
enum class TileEdge : u8 { NegativeX = 0, PositiveX, NegativeZ, PositiveZ, kCount };

inline constexpr u32 kTileEdgeCount = static_cast<u32>(TileEdge::kCount);

/// One meshed tile, in the form a geometry cook consumes.
///
/// Positions are LOCAL to the tile's minimum corner, in metres, as f32. A terrain tile a thousand
/// kilometres from the origin has 64 mm of f32 spacing in absolute coordinates and none at all in
/// local ones, which is the same argument `world::WorldPosition` makes for entities.
struct TerrainMesh {
    TileCoord coord;
    TerrainBounds bounds;
    Array<Vec3> positions;
    Array<Vec3> normals;
    Array<Vec2> uvs;
    Array<u32> indices;
    /// One material index per triangle, so clustering never mixes materials — which is what
    /// `rendering::vg::SourceMesh` wants and what keeps a cluster single-material.
    Array<u32> triangle_materials;
    /// Everything this mesh is a function of, hashed. See the header note.
    u64 key = 0;

    explicit TerrainMesh(Allocator& allocator) noexcept
        : positions(allocator),
          normals(allocator),
          uvs(allocator),
          indices(allocator),
          triangle_materials(allocator) {}

    TerrainMesh(const TerrainMesh&) = delete;
    TerrainMesh& operator=(const TerrainMesh&) = delete;
    TerrainMesh(TerrainMesh&&) noexcept = default;
    TerrainMesh& operator=(TerrainMesh&&) noexcept = default;
    ~TerrainMesh() = default;

    [[nodiscard]] usize triangle_count() const noexcept { return indices.size() / 3; }
};

/// What a meshing call was asked to do.
struct MeshOptions {
    /// The level of the tile across each edge. Equal to the tile's own level means no stitch;
    /// coarser means this tile yields along that edge. Indexed by `TileEdge`.
    u8 neighbour_level[kTileEdgeCount] = {};
    /// Include the VISUAL delta — ruts and footprints. The render path does; a collision mesh and
    /// a navigation mesh do not, which is the whole of "a footprint is cheap".
    bool include_visual = true;
    /// False where the device has no virtual geometry. "terrain SHALL fall back to a conventional
    /// tiled mesh path WITH THE REDUCED DETAIL REPORTED, exactly as other geometry does."
    bool virtual_geometry = true;
    /// How much the fallback path decimates. Two means every second sample.
    u32 fallback_decimation = 2;
};

/// What a meshing call did. Counted, not claimed.
struct MeshReport {
    u32 vertices = 0;
    u32 triangles = 0;
    /// Vertices moved onto a coarser neighbour's edge. Zero when no neighbour is coarser — and a
    /// non-zero count is the only evidence that the stitch ran at all.
    u32 stitched_vertices = 0;
    /// Quads not emitted because they are holes.
    u32 hole_quads = 0;
    bool fallback = false;
    /// Samples per edge relative to the full-detail path, where 1 is no loss.
    f32 detail = 1.0F;
    const char* limitation = "";
};

/// Mesh one tile.
///
/// `heights` is the query-side view of the cooked tile plus the gameplay delta;
/// `visual` may be null, and supplies the rendering-only displacement when `include_visual` is set.
[[nodiscard]] Expected<TerrainMesh, Error> mesh_tile(
    Allocator& allocator, const TerrainStore& store, const HeightfieldSource& heights,
    const TerrainDeltaStore* visual, const TileCoord& coord, const MeshOptions& options,
    MeshReport& report) noexcept;

/// The height a fine tile must use at one boundary sample so that its edge lies on the straight
/// segment the coarser neighbour will draw.
///
/// A free function so that meshing and a diagnostic agree about the rule. The watertightness suite
/// deliberately does NOT call it: it derives the expected boundary position from the COARSE tile's
/// own samples, because a test that asks the implementation what the answer should be is a test
/// that passes on a wrong answer.
[[nodiscard]] f32 stitch_edge(f32 low, f32 high, u32 index, u32 step) noexcept;

}  // namespace cy::terrain
