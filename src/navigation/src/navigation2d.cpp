// SPDX-License-Identifier: MIT
// The tilemap-to-polygon adapter and planar query surface.

#include <cy/navigation/navigation2d.h>

#include <algorithm>
#include <cmath>

namespace cy::navigation {
namespace {

[[nodiscard]] Vec3 lift(Vec2 point) noexcept {
    return Vec3{point.x, 0.0F, point.y};
}

[[nodiscard]] bool separated_on_axis(Span<const Vec2> polygon, Vec2 low, Vec2 high,
                                     Vec2 axis) noexcept {
    f32 polygon_min = math::kInfinity;
    f32 polygon_max = -math::kInfinity;
    for (Vec2 point : polygon) {
        const f32 projection = dot(point, axis);
        polygon_min = std::min(polygon_min, projection);
        polygon_max = std::max(polygon_max, projection);
    }
    const Vec2 corners[4] = {low, {high.x, low.y}, high, {low.x, high.y}};
    f32 cell_min = math::kInfinity;
    f32 cell_max = -math::kInfinity;
    for (Vec2 point : corners) {
        const f32 projection = dot(point, axis);
        cell_min = std::min(cell_min, projection);
        cell_max = std::max(cell_max, projection);
    }
    return polygon_max <= cell_min || cell_max <= polygon_min;
}

[[nodiscard]] bool intersects_cell(Span<const Vec2> polygon, Vec2 low, Vec2 high) noexcept {
    if (separated_on_axis(polygon, low, high, Vec2{1.0F, 0.0F}) ||
        separated_on_axis(polygon, low, high, Vec2{0.0F, 1.0F})) {
        return false;
    }
    for (usize index = 0; index < polygon.size(); ++index) {
        const Vec2 a = polygon[index];
        const Vec2 b = polygon[(index + 1) % polygon.size()];
        if (separated_on_axis(polygon, low, high, Vec2{b.y - a.y, a.x - b.x})) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_polygon(const NavPolygon2D& polygon, f32 cell_size) noexcept {
    if (polygon.corner_count < 3 || polygon.corner_count > kMaxPolyVertices ||
        polygon.area >= kAreaCount || !std::isfinite(polygon.cost) || polygon.cost <= 0.0F) {
        return false;
    }
    for (u32 index = 0; index < polygon.corner_count; ++index) {
        const Vec2 corner = polygon.corners[index];
        if (!std::isfinite(corner.x) || !std::isfinite(corner.y) || corner.x < 0.0F ||
            corner.y < 0.0F || corner.x > cell_size || corner.y > cell_size) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Status append_polygon(NavTileData& tile, const NavPolygon2D& polygon,
                                    Vec2 cell_origin, f32 cell_size) noexcept {
    if (!valid_polygon(polygon, cell_size)) {
        return fail(ErrorCode::InvalidArgument, "tilemap navigation polygon is invalid");
    }
    NavPoly output;
    output.first_corner = static_cast<u32>(tile.corners().size());
    output.corner_count = polygon.corner_count;
    output.area = polygon.area;
    output.cost = polygon.cost;
    if (Status pushed = tile.polys().push_back(output); !pushed) {
        return pushed;
    }
    for (u32 index = 0; index < polygon.corner_count; ++index) {
        const Vec3 vertex = lift(cell_origin + polygon.corners[index]);
        u32 vertex_index = static_cast<u32>(tile.vertices().size());
        const f32 tolerance = cell_size * 0.00001F;
        for (u32 existing = 0; existing < tile.vertices().size(); ++existing) {
            const Vec3 candidate = tile.vertices()[existing];
            if (std::fabs(candidate.x - vertex.x) <= tolerance &&
                std::fabs(candidate.z - vertex.z) <= tolerance) {
                vertex_index = existing;
                break;
            }
        }
        if (vertex_index == tile.vertices().size()) {
            if (Status pushed = tile.vertices().push_back(vertex); !pushed) {
                return pushed;
            }
        }
        if (Status pushed = tile.corners().push_back(vertex_index); !pushed) {
            return pushed;
        }
    }
    return ok();
}

}  // namespace

NavMesh2D::NavMesh2D(Allocator& allocator, Name name, u32 cells_per_chunk, f32 cell_size) noexcept
    : mesh_(allocator, name, static_cast<f32>(cells_per_chunk) * cell_size),
      cells_per_chunk_(cells_per_chunk),
      cell_size_(cell_size) {}

Status NavMesh2D::rebuild_chunk(TileCoord chunk, Span<const TilemapNavCell> cells) noexcept {
    if (cells_per_chunk_ == 0 || !std::isfinite(cell_size_) || cell_size_ <= 0.0F) {
        return fail(ErrorCode::InvalidArgument, "tilemap navigation chunk layout is invalid");
    }
    NavTileData tile(mesh_.allocator());
    tile.coord = chunk;
    const f32 chunk_size = static_cast<f32>(cells_per_chunk_) * cell_size_;
    const Vec2 chunk_origin{static_cast<f32>(chunk.x) * chunk_size,
                            static_cast<f32>(chunk.z) * chunk_size};
    for (const TilemapNavCell& cell : cells) {
        if (cell.x >= cells_per_chunk_ || cell.y >= cells_per_chunk_) {
            return fail(ErrorCode::OutOfRange, "tilemap navigation cell is outside its chunk");
        }
        const Vec2 origin = chunk_origin + Vec2{static_cast<f32>(cell.x) * cell_size_,
                                                static_cast<f32>(cell.y) * cell_size_};
        for (const NavPolygon2D& polygon : cell.polygons) {
            if (Status added = append_polygon(tile, polygon, origin, cell_size_); !added) {
                return added;
            }
        }
    }
    if (tile.polys().empty()) {
        if (!mesh_.tile_resident(chunk)) {
            return ok();
        }
        return mesh_.remove_tile(chunk);
    }
    tile.finalise();
    const Expected<TileChange, Error> published = mesh_.add_tile(std::move(tile));
    if (!published) {
        return make_unexpected(published.error());
    }
    return ok();
}

Status NavMesh2D::remove_chunk(TileCoord chunk) noexcept {
    return mesh_.remove_tile(chunk);
}

Status NavMesh2D::rebuild_chunk_from_collision(TileCoord chunk,
                                               Span<const CollisionPolygon2D> colliders) noexcept {
    if (cells_per_chunk_ == 0 || !std::isfinite(cell_size_) || cell_size_ <= 0.0F) {
        return fail(ErrorCode::InvalidArgument, "tilemap navigation chunk layout is invalid");
    }
    for (const CollisionPolygon2D& collider : colliders) {
        if (collider.vertices.size() < 3) {
            return fail(ErrorCode::InvalidArgument, "2D collision polygon needs three vertices");
        }
        for (Vec2 vertex : collider.vertices) {
            if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y)) {
                return fail(ErrorCode::InvalidArgument,
                            "2D collision polygon has a nonfinite vertex");
            }
        }
    }

    NavPolygon2D square;
    square.corner_count = 4;
    square.corners[0] = Vec2{0.0F, 0.0F};
    square.corners[1] = Vec2{cell_size_, 0.0F};
    square.corners[2] = Vec2{cell_size_, cell_size_};
    square.corners[3] = Vec2{0.0F, cell_size_};
    Array<TilemapNavCell> cells(mesh_.allocator());
    const f32 chunk_size = static_cast<f32>(cells_per_chunk_) * cell_size_;
    const Vec2 origin{static_cast<f32>(chunk.x) * chunk_size,
                      static_cast<f32>(chunk.z) * chunk_size};
    for (u32 y = 0; y < cells_per_chunk_; ++y) {
        for (u32 x = 0; x < cells_per_chunk_; ++x) {
            const Vec2 low =
                origin + Vec2{static_cast<f32>(x) * cell_size_, static_cast<f32>(y) * cell_size_};
            const Vec2 high = low + Vec2{cell_size_, cell_size_};
            bool blocked = false;
            for (const CollisionPolygon2D& collider : colliders) {
                if (intersects_cell(collider.vertices, low, high)) {
                    blocked = true;
                    break;
                }
            }
            if (!blocked) {
                if (Status pushed =
                        cells.push_back(TilemapNavCell{x, y, Span<const NavPolygon2D>(&square, 1)});
                    !pushed) {
                    return pushed;
                }
            }
        }
    }
    return rebuild_chunk(chunk, cells.span());
}

PathResult NavMesh2D::find_path(Vec2 start, Vec2 end, Vec2 extents, const PathFilter& filter,
                                PathCorridor& corridor) const noexcept {
    return navigation::find_path(mesh_, lift(start), lift(end), Vec3{extents.x, 0.25F, extents.y},
                                 filter, corridor);
}

Status NavMesh2D::straighten(const PathCorridor& corridor, Vec2 start, Vec2 end,
                             Array<Vec2>& points) const noexcept {
    Array<PathPoint> path(mesh_.allocator());
    if (Status result = navigation::straighten(mesh_, corridor, lift(start), lift(end), path);
        !result) {
        return result;
    }
    points.clear();
    for (const PathPoint& point : path.span()) {
        if (Status pushed = points.push_back(Vec2{point.position.x, point.position.z}); !pushed) {
            return pushed;
        }
    }
    return ok();
}

}  // namespace cy::navigation
