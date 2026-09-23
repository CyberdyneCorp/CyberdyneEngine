#pragma once
// Planar navigation uses the same tiled polygon graph and A* search as surface navigation. A
// tilemap contributes its authored navigation polygons chunk by chunk; a changed chunk replaces
// only its own tile, preserving references into every other chunk.

#include <cy/navigation/query.h>

namespace cy::navigation {

/// A convex polygon in tile-local, Y-down coordinates. Six vertices is the runtime mesh limit.
struct NavPolygon2D {
    Vec2 corners[kMaxPolyVertices] = {};
    u8 corner_count = 0;
    AreaType area = kAreaGround;
    f32 cost = 1.0F;
};

/// One tilemap cell and the navigation polygons its tileset declares. Polygons use cell-local
/// coordinates; empty cells contribute no polygons. The spans are consumed during rebuild.
struct TilemapNavCell {
    u32 x = 0;
    u32 y = 0;
    Span<const NavPolygon2D> polygons;
};

/// Convex 2D collision footprint in world coordinates. The conservative collision builder
/// excludes any navigation cell whose interior overlaps one of these polygons.
struct CollisionPolygon2D {
    Span<const Vec2> vertices;
};

class NavMesh2D {
public:
    NavMesh2D(Allocator& allocator, Name name, u32 cells_per_chunk, f32 cell_size) noexcept;

    [[nodiscard]] u32 cells_per_chunk() const noexcept { return cells_per_chunk_; }
    [[nodiscard]] f32 cell_size() const noexcept { return cell_size_; }
    [[nodiscard]] u32 version() const noexcept { return mesh_.version(); }
    [[nodiscard]] u32 chunk_count() const noexcept { return mesh_.tile_count(); }

    /// Replaces exactly one tilemap chunk. The same chunk coordinate can be rebuilt on a cell
    /// change; references into neighbouring chunks remain valid.
    [[nodiscard]] Status rebuild_chunk(TileCoord chunk, Span<const TilemapNavCell> cells) noexcept;
    /// Generate planar polygons from collision footprints on a cell grid. This is deliberately
    /// conservative: a partially covered cell is not navigable.
    [[nodiscard]] Status rebuild_chunk_from_collision(
        TileCoord chunk, Span<const CollisionPolygon2D> colliders) noexcept;
    [[nodiscard]] Status remove_chunk(TileCoord chunk) noexcept;

    /// The public path surface is planar. A corridor carries only polygon identities, and points
    /// returned by straighten are Vec2 in the same Y-down coordinates as the tilemap.
    [[nodiscard]] PathResult find_path(Vec2 start, Vec2 end, Vec2 extents, const PathFilter& filter,
                                       PathCorridor& corridor) const noexcept;
    [[nodiscard]] Status straighten(const PathCorridor& corridor, Vec2 start, Vec2 end,
                                    Array<Vec2>& points) const noexcept;

private:
    NavMesh mesh_;
    u32 cells_per_chunk_ = 0;
    f32 cell_size_ = 0.0F;
};

}  // namespace cy::navigation
