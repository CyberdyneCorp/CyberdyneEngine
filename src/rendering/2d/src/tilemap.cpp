// Chunked tile storage, the four grid shapes, and the terrain solver. M8.b task 9.5.

#include <cy/rendering/2d/tilemap.h>

#include <cmath>

namespace cy::rendering2d {
namespace {

[[nodiscard]] i32 floor_div(i32 value, i32 divisor) noexcept {
    const i32 quotient = value / divisor;
    return ((value % divisor != 0) && ((value < 0) != (divisor < 0))) ? (quotient - 1) : quotient;
}

[[nodiscard]] i32 positive_mod(i32 value, i32 divisor) noexcept {
    const i32 remainder = value % divisor;
    return (remainder < 0) ? (remainder + divisor) : remainder;
}

}  // namespace

const char* grid_shape_name(GridShape shape) noexcept {
    switch (shape) {
        case GridShape::Square:
            return "square";
        case GridShape::Isometric:
            return "isometric";
        case GridShape::HexagonalFlatTop:
            return "hexagonal-flat-top";
        case GridShape::HexagonalPointyTop:
            return "hexagonal-pointy-top";
        case GridShape::HalfOffsetSquare:
            return "half-offset-square";
        case GridShape::Count:
            break;
    }
    return "unknown";
}

Status Tileset::add(const Tile& tile) noexcept {
    for (Tile& existing : tiles_.span()) {
        if (existing.id == tile.id) {
            existing = tile;
            return ok();
        }
    }
    return tiles_.push_back(tile);
}

const Tile* Tileset::find(u16 id) const noexcept {
    for (const Tile& tile : tiles_.span()) {
        if (tile.id == id) {
            return &tile;
        }
    }
    return nullptr;
}

Vec2 cell_origin(GridShape shape, Vec2 tile_size, i32 x, i32 y) noexcept {
    const f32 fx = static_cast<f32>(x);
    const f32 fy = static_cast<f32>(y);
    switch (shape) {
        case GridShape::Square:
            return Vec2{fx * tile_size.x, fy * tile_size.y};
        case GridShape::Isometric:
            // The diamond: a step in x moves half a tile right and half a tile down, and a step in
            // y moves half a tile left and half a tile down.
            return Vec2{(fx - fy) * tile_size.x * 0.5F, (fx + fy) * tile_size.y * 0.5F};
        case GridShape::HexagonalFlatTop:
            // Columns are three quarters of a tile apart, and every other column drops half a tile.
            return Vec2{
                fx * tile_size.x * 0.75F,
                (fy * tile_size.y) + ((positive_mod(x, 2) != 0) ? (tile_size.y * 0.5F) : 0.0F)};
        case GridShape::HexagonalPointyTop:
            return Vec2{
                (fx * tile_size.x) + ((positive_mod(y, 2) != 0) ? (tile_size.x * 0.5F) : 0.0F),
                fy * tile_size.y * 0.75F};
        case GridShape::HalfOffsetSquare:
            return Vec2{
                (fx * tile_size.x) + ((positive_mod(y, 2) != 0) ? (tile_size.x * 0.5F) : 0.0F),
                fy * tile_size.y};
        case GridShape::Count:
            break;
    }
    return Vec2{fx * tile_size.x, fy * tile_size.y};
}

ChunkCoord chunk_of(i32 x, i32 y) noexcept {
    return ChunkCoord{floor_div(x, kChunkSide), floor_div(y, kChunkSide)};
}

TileLayer::TileLayer(Allocator& allocator, GridShape shape) noexcept
    : chunks_(allocator), shape_(shape) {}

i64 TileLayer::find_chunk(ChunkCoord coord) const noexcept {
    for (usize index = 0; index < chunks_.size(); ++index) {
        if (chunks_[index].coord == coord) {
            return static_cast<i64>(index);
        }
    }
    return -1;
}

Expected<usize, Error> TileLayer::chunk_for(i32 x, i32 y) noexcept {
    const ChunkCoord coord = chunk_of(x, y);
    const i64 found = find_chunk(coord);
    if (found >= 0) {
        return static_cast<usize>(found);
    }
    // SPARSE: a chunk exists only where cells do. A world a thousand chunks wide with content in
    // four of them holds four.
    Chunk chunk;
    chunk.coord = coord;
    if (Status pushed = chunks_.push_back(chunk); !pushed) {
        return make_unexpected(pushed.error());
    }
    return chunks_.size() - 1;
}

Status TileLayer::set_cell(i32 x, i32 y, const Cell& cell) noexcept {
    auto index = chunk_for(x, y);
    if (!index) {
        return make_unexpected(index.error());
    }
    Chunk& chunk = chunks_[index.value()];
    const i32 local_x = positive_mod(x, kChunkSide);
    const i32 local_y = positive_mod(y, kChunkSide);
    Cell& slot = chunk.cells[(local_y * kChunkSide) + local_x];
    if (slot.tile == 0 && cell.tile != 0) {
        ++chunk.live;
    } else if (slot.tile != 0 && cell.tile == 0) {
        --chunk.live;
    }
    slot = cell;
    // EXACTLY ONE CHUNK. "WHEN one cell changes THEN only its chunk's geometry, collision, and
    // navigation SHALL be rebuilt."
    chunk.dirty.geometry = true;
    chunk.dirty.instances = true;
    return ok();
}

const Cell* TileLayer::cell(i32 x, i32 y) const noexcept {
    const i64 index = find_chunk(chunk_of(x, y));
    if (index < 0) {
        return nullptr;
    }
    const Chunk& chunk = chunks_[static_cast<usize>(index)];
    const i32 local_x = positive_mod(x, kChunkSide);
    const i32 local_y = positive_mod(y, kChunkSide);
    const Cell& slot = chunk.cells[(local_y * kChunkSide) + local_x];
    return (slot.tile == 0) ? nullptr : &slot;
}

Status TileLayer::clear_cell(i32 x, i32 y) noexcept {
    return set_cell(x, y, Cell{});
}

Status TileLayer::override_cell(i32 x, i32 y, const Cell& cell) noexcept {
    Cell overridden = cell;
    overridden.overridden = true;
    return set_cell(x, y, overridden);
}

void TileLayer::touch_animation(ChunkCoord coord) noexcept {
    const i64 index = find_chunk(coord);
    if (index < 0) {
        return;
    }
    // INSTANCES ONLY. An animated tile re-submits its chunk with a new frame index and does not
    // rebuild its geometry — which is also why its collision and navigation are untouched.
    chunks_[static_cast<usize>(index)].dirty.instances = true;
}

ChunkDirty TileLayer::dirty_of(ChunkCoord coord) const noexcept {
    const i64 index = find_chunk(coord);
    return (index < 0) ? ChunkDirty{} : chunks_[static_cast<usize>(index)].dirty;
}

Status TileLayer::rebuild(const Tileset& tileset, const Layer2D& layer, f32 frame_seconds,
                          Array<Draw2D>& draws, Array<ChunkBuild>& builds,
                          TilemapReport& report) noexcept {
    report = TilemapReport{};
    report.chunks = static_cast<u32>(chunks_.size());
    elapsed_ += (frame_seconds > 0.0F) ? frame_seconds : 0.0F;
    builds.clear();

    for (Chunk& chunk : chunks_.span()) {
        if (!chunk.dirty.geometry && !chunk.dirty.instances) {
            continue;  // Nothing changed here: the previous frame's draws stand.
        }
        if (chunk.dirty.geometry) {
            ++report.rebuilt_geometry;
        }
        if (chunk.dirty.instances) {
            ++report.rebuilt_instances;
        }

        ChunkBuild build;
        build.coord = chunk.coord;
        build.first_draw = static_cast<u32>(draws.size());

        for (i32 local_y = 0; local_y < kChunkSide; ++local_y) {
            for (i32 local_x = 0; local_x < kChunkSide; ++local_x) {
                const Cell& cell = chunk.cells[(local_y * kChunkSide) + local_x];
                if (cell.tile == 0) {
                    continue;
                }
                const Tile* tile = tileset.find(cell.tile);
                if (tile == nullptr) {
                    continue;
                }
                ++report.cells;

                Rect2D source = tile->source;
                if (tile->animation_frames > 1) {
                    // The frame index moves; the geometry does not.
                    const auto frame = static_cast<u16>(
                        static_cast<u32>(elapsed_ / ((tile->animation_seconds > 0.0F)
                                                         ? tile->animation_seconds
                                                         : 0.1F)) %
                        tile->animation_frames);
                    const Tile* animated =
                        tileset.find(static_cast<u16>(tile->animation_first + frame));
                    if (animated != nullptr) {
                        source = animated->source;
                    }
                }

                Draw2D draw;
                draw.kind = PrimitiveKind::Sprite;
                draw.sort.layer = layer.index;
                draw.sort.tiebreak =
                    static_cast<u32>(((chunk.coord.y * 1024) + chunk.coord.x) & 0xFFF);
                const i32 cell_x = (chunk.coord.x * kChunkSide) + local_x;
                const i32 cell_y = (chunk.coord.y * kChunkSide) + local_y;
                const Vec2 origin = cell_origin(shape_, tileset.tile_size, cell_x, cell_y);
                draw.sort.y_sort = origin.y;
                draw.destination =
                    Rect2D{origin.x, origin.y, tileset.tile_size.x, tileset.tile_size.y};
                draw.source = source;
                draw.texture_set = tileset.texture_set;
                draw.flip_x = cell.variant.flip_x;
                draw.flip_y = cell.variant.flip_y;
                draw.rotation = static_cast<f32>(cell.variant.rotation) * 1.5707963F;
                if (Status pushed = draws.push_back(draw); !pushed) {
                    return pushed;
                }

                // The shapes a chunk carries. Registering them with the physics and navigation
                // servers is the CALLER's: a rendering module may not name either, and counting
                // them here is what lets a rebuild be reported.
                build.collision_shapes += (tile->collision_shape != 0) ? 1U : 0U;
                build.navigation_shapes += (tile->navigation_shape != 0) ? 1U : 0U;
                build.occlusion_shapes += (tile->occlusion_shape != 0) ? 1U : 0U;
            }
        }
        build.draw_count = static_cast<u32>(draws.size()) - build.first_draw;
        if (Status pushed = builds.push_back(build); !pushed) {
            return pushed;
        }
        chunk.dirty = ChunkDirty{};
    }
    report.draws = static_cast<u32>(draws.size());
    return ok();
}

u16 solve_terrain(const Tileset& tileset, const TerrainCorners& wanted, bool& exact) noexcept {
    exact = false;
    u16 best = 0;
    u32 best_score = 0;
    for (const Tile& tile : tileset.tiles()) {
        u32 score = 0;
        for (u32 corner = 0; corner < 4U; ++corner) {
            if (tile.corners[corner] == wanted.corners[corner]) {
                ++score;
            }
        }
        if (score == 4U) {
            exact = true;
            return tile.id;
        }
        if (score > best_score) {
            best_score = score;
            best = tile.id;
        }
    }
    // NO EXACT MATCH: the closest tile, and `exact` says so. A tileset with a hole in it should
    // produce a visible seam rather than nothing at all, and the caller decides which.
    return best;
}

Status paint_terrain(TileLayer& layer, const Tileset& tileset, i32 x, i32 y, i32 width, i32 height,
                     u8 terrain) noexcept {
    if (width <= 0 || height <= 0) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a terrain brush needs a positive extent", 0});
    }
    // Two passes: mark membership, then solve every cell whose corners the marking could have
    // changed — the painted rectangle plus one cell of margin, because a cell outside the brush
    // shares corners with one inside it and its tile has to change too.
    for (i32 row = y; row < y + height; ++row) {
        for (i32 column = x; column < x + width; ++column) {
            Cell cell;
            cell.tile = 1;  // provisional; the solve below replaces it
            if (Status set = layer.set_cell(column, row, cell); !set) {
                return set;
            }
        }
    }
    for (i32 row = y - 1; row <= y + height; ++row) {
        for (i32 column = x - 1; column <= x + width; ++column) {
            if (layer.cell(column, row) == nullptr) {
                continue;
            }
            TerrainCorners wanted;
            // A corner belongs to the terrain when the cell touching it does. The four corners are
            // top-left, top-right, bottom-right, bottom-left, which is the order `Tile::corners`
            // declares — and getting that order wrong is how autotiling produces a seam that only
            // appears diagonally.
            const auto inside = [&](i32 cell_x, i32 cell_y) noexcept {
                return cell_x >= x && cell_x < x + width && cell_y >= y && cell_y < y + height;
            };
            wanted.corners[0] = inside(column - 1, row - 1) || inside(column, row) ? terrain : 0;
            wanted.corners[1] = inside(column + 1, row - 1) || inside(column, row) ? terrain : 0;
            wanted.corners[2] = inside(column + 1, row + 1) || inside(column, row) ? terrain : 0;
            wanted.corners[3] = inside(column - 1, row + 1) || inside(column, row) ? terrain : 0;

            bool exact = false;
            const u16 tile = solve_terrain(tileset, wanted, exact);
            if (tile == 0) {
                continue;
            }
            Cell cell;
            cell.tile = tile;
            if (Status set = layer.set_cell(column, row, cell); !set) {
                return set;
            }
        }
    }
    return ok();
}

}  // namespace cy::rendering2d
