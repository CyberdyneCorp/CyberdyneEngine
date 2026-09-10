#ifndef CY_RENDERING_2D_TILEMAP_H
#define CY_RENDERING_2D_TILEMAP_H
// Tilesets, chunked sparse layers, grid shapes, autotiling and per-chunk rebuilds. M8.b task 9.5.
//
// `rendering-2d` asks for six things and this header is those six: tilesets with per-tile data,
// chunked sparse cell storage with variants, four grid shapes, chunked rendering rebuilt only when
// dirty, constraint-based terrain autotiling, and runtime cell overrides.
//
// --- THE CHUNK IS THE UNIT OF WORK, AND THAT IS THE REQUIREMENT ----------------------------------
//
// "WHEN one cell changes THEN only its chunk's geometry, collision, and navigation SHALL be
// rebuilt." So a `TileLayer` is a sparse map of chunks, a write marks ONE chunk dirty, and
// `rebuild()` visits only the dirty ones and says how many — which is what makes the scenario a
// number rather than a claim.
//
// And the animation case is the one that shows the separation is real: "WHEN a tile declares
// animation frames THEN its chunk SHALL be re-submitted with an updated frame index WITHOUT
// rebuilding geometry." An animated tile therefore dirties the chunk's INSTANCE DATA and not its
// geometry, and those are two flags rather than one.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/rendering/2d/sprite.h>

namespace cy::rendering2d {

/// The grid a tilemap is laid out on. Four shapes, and the arithmetic for each is in `cell_origin`.
enum class GridShape : u8 {
    Square = 0,
    Isometric,
    HexagonalFlatTop,
    HexagonalPointyTop,
    /// Square cells with every other row offset by half a cell. A common 2D idiom, and cheaper than
    /// a real hexagonal grid when all it has to do is look staggered.
    HalfOffsetSquare,
    Count,
};

[[nodiscard]] const char* grid_shape_name(GridShape shape) noexcept;

/// A tile's variant: how the cell's tile is transformed. Packed into the cell so a variant costs no
/// storage of its own.
struct TileVariant {
    bool flip_x = false;
    bool flip_y = false;
    /// A quarter turn count, 0 to 3.
    u8 rotation = 0;
    /// An alternative tile from the same entry — a grass tile with three looks.
    u8 alternative = 0;
};

/// One tile in a tileset, and everything a tile carries beyond its picture.
struct Tile {
    u16 id = 0;
    /// The atlas rectangle.
    Rect2D source;
    /// Which terrain each corner belongs to, for autotiling. Zero is "no terrain".
    /// Order: top-left, top-right, bottom-right, bottom-left.
    u8 corners[4] = {};
    /// Collision shapes, navigation regions and occlusion shapes are polygon indices into the
    /// tileset's shape array — they are DATA here, and registering them with the physics and
    /// navigation servers is the caller's, because a rendering module may not name either.
    u16 collision_shape = 0;
    u16 navigation_shape = 0;
    u16 occlusion_shape = 0;
    /// Animation: the first frame's tile id and how many follow. Zero frames means static.
    u16 animation_first = 0;
    u8 animation_frames = 0;
    /// Seconds per frame.
    f32 animation_seconds = 0.1F;
    /// A project's own typed data, by name. One value; a tile needing a struct declares a component
    /// on the entity a cell spawns.
    Name custom;
};

/// A tileset: the atlas, the tiles, and the terrains they belong to.
class Tileset {
public:
    explicit Tileset(Allocator& allocator) noexcept : tiles_(allocator) {}

    [[nodiscard]] Status add(const Tile& tile) noexcept;
    [[nodiscard]] const Tile* find(u16 id) const noexcept;
    [[nodiscard]] Span<const Tile> tiles() const noexcept { return tiles_.span(); }
    [[nodiscard]] usize size() const noexcept { return tiles_.size(); }

    u16 texture_set = 0;
    Vec2 tile_size{16.0F, 16.0F};

private:
    Array<Tile> tiles_;
};

/// One cell: which tile, and which variant of it.
struct Cell {
    u16 tile = 0;
    TileVariant variant;
    /// A runtime override replaces the tile without editing the tileset — "Runtime cell overrides —
    /// per-cell modifications without editing the tileset".
    bool overridden = false;
};

/// The side of a chunk, in cells. A chunk is one draw, so it wants to be big; it is also the unit
/// of rebuild, so it wants to be small. Sixteen is the usual compromise and is a constant rather
/// than a parameter because two chunk sizes in one world would make a rebuild's cost unpredictable.
inline constexpr i32 kChunkSide = 16;

/// A chunk's coordinates, in chunks.
struct ChunkCoord {
    i32 x = 0;
    i32 y = 0;

    friend constexpr bool operator==(ChunkCoord a, ChunkCoord b) noexcept {
        return a.x == b.x && a.y == b.y;
    }
};

/// What a chunk needs rebuilding. TWO FLAGS, not one: an animated tile changes the instance data
/// and not the geometry, and conflating them would rebuild a chunk's collision every tenth of a
/// second because a torch is flickering.
struct ChunkDirty {
    bool geometry = false;
    bool instances = false;
};

/// A chunk's rebuilt output.
struct ChunkBuild {
    ChunkCoord coord;
    u32 first_draw = 0;
    u32 draw_count = 0;
    /// How many cells carried a collision, navigation or occlusion shape. The caller registers them
    /// with the servers; this module counts them so a rebuild can be reported.
    u32 collision_shapes = 0;
    u32 navigation_shapes = 0;
    u32 occlusion_shapes = 0;
};

struct TilemapReport {
    u32 chunks = 0;
    u32 rebuilt_geometry = 0;
    u32 rebuilt_instances = 0;
    u32 cells = 0;
    u32 draws = 0;
};

/// A sparse, chunked layer of cells.
class TileLayer {
public:
    TileLayer(Allocator& allocator, GridShape shape) noexcept;

    TileLayer(const TileLayer&) = delete;
    TileLayer& operator=(const TileLayer&) = delete;

    /// Write a cell. Marks exactly one chunk's GEOMETRY dirty.
    [[nodiscard]] Status set_cell(i32 x, i32 y, const Cell& cell) noexcept;
    [[nodiscard]] const Cell* cell(i32 x, i32 y) const noexcept;
    [[nodiscard]] Status clear_cell(i32 x, i32 y) noexcept;
    /// A runtime override: the same as `set_cell` but recorded as an override, so a reload of the
    /// authored map can tell the two apart.
    [[nodiscard]] Status override_cell(i32 x, i32 y, const Cell& cell) noexcept;

    /// Mark a chunk's instance data dirty without touching its geometry — what an animated tile
    /// does, and the reason `ChunkDirty` has two flags.
    void touch_animation(ChunkCoord coord) noexcept;

    [[nodiscard]] usize chunk_count() const noexcept { return chunks_.size(); }
    [[nodiscard]] ChunkDirty dirty_of(ChunkCoord coord) const noexcept;
    [[nodiscard]] GridShape shape() const noexcept { return shape_; }

    /// Rebuild whatever is dirty into draws. `frame_seconds` advances animated tiles.
    [[nodiscard]] Status rebuild(const Tileset& tileset, const Layer2D& layer, f32 frame_seconds,
                                 Array<Draw2D>& draws, Array<ChunkBuild>& builds,
                                 TilemapReport& report) noexcept;

private:
    struct Chunk {
        ChunkCoord coord;
        Cell cells[kChunkSide * kChunkSide];
        ChunkDirty dirty;
        u32 live = 0;
    };

    [[nodiscard]] i64 find_chunk(ChunkCoord coord) const noexcept;
    [[nodiscard]] Expected<usize, Error> chunk_for(i32 x, i32 y) noexcept;

    Array<Chunk> chunks_;
    GridShape shape_ = GridShape::Square;
    f32 elapsed_ = 0.0F;
};

/// A cell's origin in world units, for a grid shape and a tile size. The four shapes' arithmetic,
/// in one function, so a caller never writes it twice.
[[nodiscard]] Vec2 cell_origin(GridShape shape, Vec2 tile_size, i32 x, i32 y) noexcept;

/// Which chunk a cell belongs to. Exposed because a caller invalidating a region wants it.
[[nodiscard]] ChunkCoord chunk_of(i32 x, i32 y) noexcept;

// --- Terrain autotiling
// ---------------------------------------------------------------------------

/// A cell's four corners' terrain membership, as the solver sees it.
struct TerrainCorners {
    u8 corners[4] = {};

    friend constexpr bool operator==(const TerrainCorners& a, const TerrainCorners& b) noexcept {
        return a.corners[0] == b.corners[0] && a.corners[1] == b.corners[1] &&
               a.corners[2] == b.corners[2] && a.corners[3] == b.corners[3];
    }
};

/// Choose the tile whose corners match, and say how well.
///
/// "WHEN cells are painted with a terrain in corner-and-edge mode THEN the solver SHALL choose
/// tiles so all shared corners and edges match, producing seamless transitions." An exact match
/// wins; when there is none the closest is chosen and `exact` says so, because a tileset with a
/// hole in it should produce a visible seam rather than a crash.
[[nodiscard]] u16 solve_terrain(const Tileset& tileset, const TerrainCorners& wanted,
                                bool& exact) noexcept;

/// Paint a rectangle of cells with a terrain, resolving every cell's corners against its
/// neighbours.
///
/// This is where "resolved when cells are painted" happens: the solve is at paint time and the
/// runtime draws whatever the cells say, which is what keeps the renderer's inner loop free of it.
[[nodiscard]] Status paint_terrain(TileLayer& layer, const Tileset& tileset, i32 x, i32 y,
                                   i32 width, i32 height, u8 terrain) noexcept;

}  // namespace cy::rendering2d

#endif  // CY_RENDERING_2D_TILEMAP_H
