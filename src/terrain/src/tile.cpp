// Tile identity, the layout's arithmetic, what a tile carries, and the sparse hierarchy that holds
// them. M10 task 2.1.

#include <cy/terrain/tile.h>

#include <bit>
#include <cmath>

namespace cy::terrain {
namespace {

/// Floor division for a positive divisor. `a / b` truncates toward zero, which puts the tile
/// boundary at the origin in the wrong place for negative coordinates — a world west of its origin
/// would fold onto itself. src/environment/src/store.cpp carries the same function for the same
/// reason; it is four lines and neither module should reach into the other's translation unit.
[[nodiscard]] i64 floor_div(i64 value, i64 divisor) noexcept {
    const i64 quotient = value / divisor;
    return ((value % divisor != 0) && ((value < 0) != (divisor < 0))) ? quotient - 1 : quotient;
}

[[nodiscard]] u64 tile_key(const TileCoord& coord) noexcept {
    // Not the tile IDENTITY: this is the store's own key, and it has to be derived from the
    // coordinate alone so a lookup does not need the layout. Identity is `tile_id_of()`.
    u64 value = hash_integer(static_cast<u64>(static_cast<u32>(coord.x)), kTerrainHashSeed);
    value = hash_combine(value, static_cast<u64>(static_cast<u32>(coord.z)));
    return hash_combine(value, coord.level);
}

[[nodiscard]] f32 clamp_f32(f32 value, f32 low, f32 high) noexcept {
    if (value < low) {
        return low;
    }
    return (value > high) ? high : value;
}

}  // namespace

const char* coordinate_scheme_name(CoordinateScheme scheme) noexcept {
    switch (scheme) {
        case CoordinateScheme::PlanarGrid:
            return "planar-grid";
        case CoordinateScheme::CubeFaceQuadtree:
            return "cube-face-quadtree";
    }
    return "unknown";
}

const char* detail_reason_name(DetailReason reason) noexcept {
    switch (reason) {
        case DetailReason::Resident:
            return "resident";
        case DetailReason::Streaming:
            return "streaming";
        case DetailReason::Budget:
            return "budget";
        case DetailReason::Distance:
            return "distance";
        case DetailReason::MissingRepresentation:
            return "missing-representation";
    }
    return "unknown";
}

// --- The layout
// -----------------------------------------------------------------------------------

f64 TileLayout::tile_size(u8 level) const noexcept {
    f64 size = static_cast<f64>(tile_metres);
    for (u8 step = 0; step < level; ++step) {
        size *= static_cast<f64>(level_ratio);
    }
    return size;
}

f32 TileLayout::sample_metres(u8 level) const noexcept {
    return static_cast<f32>(tile_size(level) / static_cast<f64>(kTileQuads));
}

u64 TileLayout::signature() const noexcept {
    // Every field, including the scheme and the face: two layouts that differ anywhere name
    // different tiles for the same piece of world, which is what makes a stale streaming cache
    // report a mismatch rather than answer for a different world. Fixed-seed, so a cooker and a
    // runtime on two machines agree.
    u64 value = hash_integer(static_cast<u64>(scheme), kTerrainHashSeed);
    value = hash_combine(value, terrain);
    value = hash_combine(value, face);
    value = hash_combine(value, static_cast<u64>(std::bit_cast<u64>(origin_x)));
    value = hash_combine(value, static_cast<u64>(std::bit_cast<u64>(origin_z)));
    value = hash_combine(value, static_cast<u64>(std::bit_cast<u32>(tile_metres)));
    value = hash_combine(value, levels);
    value = hash_combine(value, level_ratio);
    value = hash_combine(value, static_cast<u64>(std::bit_cast<u32>(height_min)));
    return hash_combine(value, static_cast<u64>(std::bit_cast<u32>(height_max)));
}

bool TileLayout::is_valid() const noexcept {
    return levels > 0 && levels <= kMaxTerrainLevels && tile_metres > 0.0F && level_ratio >= 2 &&
           height_max > height_min;
}

TileId tile_id_of(const TileLayout& layout, const TileCoord& coord) noexcept {
    u64 value = hash_integer(layout.signature(), kTerrainHashSeed);
    value = hash_combine(value, coord.level);
    value = hash_combine(value, static_cast<u64>(static_cast<u32>(coord.x)));
    value = hash_combine(value, static_cast<u64>(static_cast<u32>(coord.z)));
    // Zero is the invalid identifier, and a hash may legitimately produce it. One collision of
    // value with a neighbour is harmless; a tile that reports `is_valid() == false` is not.
    return TileId{value == 0 ? 1 : value};
}

TileCoord tile_at(const TileLayout& layout, f64 x, f64 z, u8 level) noexcept {
    const f64 size = layout.tile_size(level);
    TileCoord coord;
    coord.x = static_cast<i32>(std::floor((x - layout.origin_x) / size));
    coord.z = static_cast<i32>(std::floor((z - layout.origin_z) / size));
    coord.level = level;
    return coord;
}

TerrainBounds tile_bounds(const TileLayout& layout, const TileCoord& coord) noexcept {
    const f64 size = layout.tile_size(coord.level);
    TerrainBounds bounds;
    bounds.min_x = layout.origin_x + (static_cast<f64>(coord.x) * size);
    bounds.min_z = layout.origin_z + (static_cast<f64>(coord.z) * size);
    bounds.max_x = bounds.min_x + size;
    bounds.max_z = bounds.min_z + size;
    return bounds;
}

TileCoord parent_of(const TileLayout& layout, const TileCoord& coord) noexcept {
    const i64 ratio = static_cast<i64>(layout.level_ratio);
    TileCoord parent;
    parent.x = static_cast<i32>(floor_div(coord.x, ratio));
    parent.z = static_cast<i32>(floor_div(coord.z, ratio));
    parent.level = static_cast<u8>(coord.level + 1);
    return parent;
}

TerrainPoint sample_position(const TileLayout& layout, const TileCoord& coord, u32 i,
                             u32 j) noexcept {
    // The ABSOLUTE lattice index, not the tile's corner plus an offset. Tile A's last sample and
    // tile B's first sample are the same point in the world and must be the same f64: computed as
    // `min_x + i * spacing` they differ by an ulp or two for a tile size that is not a power of
    // two, and a crater stamped across the boundary would then move the two tiles' shared row by
    // different amounts — a crack that appears only where a deformation crosses a seam, which is
    // the worst kind to find later. One multiplication from a shared integer index makes them
    // identical by construction.
    const f64 spacing = layout.tile_size(coord.level) / static_cast<f64>(kTileQuads);
    const f64 lattice_i = static_cast<f64>((static_cast<i64>(coord.x) * kTileQuads) + i);
    const f64 lattice_j = static_cast<f64>((static_cast<i64>(coord.z) * kTileQuads) + j);
    return TerrainPoint{layout.origin_x + (lattice_i * spacing),
                        layout.origin_z + (lattice_j * spacing)};
}

u16 quantise_height(const TileLayout& layout, f32 metres) noexcept {
    const f32 span = layout.height_max - layout.height_min;
    const f32 unit = clamp_f32((metres - layout.height_min) / span, 0.0F, 1.0F);
    // Round to nearest. A truncating quantiser biases every cooked world downward by half a step,
    // which is invisible in one tile and is a systematic error across a continent.
    return static_cast<u16>(std::lround(unit * 65535.0F));
}

f32 dequantise_height(const TileLayout& layout, u16 stored) noexcept {
    const f32 span = layout.height_max - layout.height_min;
    return layout.height_min + ((static_cast<f32>(stored) / 65535.0F) * span);
}

// --- What a tile carries
// --------------------------------------------------------------------------

u32 MaterialTexel::used() const noexcept {
    u32 count = 0;
    for (const u8 slot : weight) {
        if (slot != 0) {
            ++count;
        }
    }
    return count;
}

Expected<TerrainTile, Error> TerrainTile::create(Allocator& allocator, const TileLayout& layout,
                                                 const TileCoord& coord, f32 height) noexcept {
    if (!layout.is_valid()) {
        return fail(ErrorCode::InvalidArgument, "terrain: the tile layout does not name tiles");
    }
    if (coord.level >= layout.levels) {
        return fail(ErrorCode::OutOfRange, "terrain: that level is not declared by the layout");
    }

    TerrainTile tile(allocator);
    tile.coord = coord;
    tile.id = tile_id_of(layout, coord);
    if (Status sized = tile.heights.resize(kTileHeights); !sized) {
        return make_unexpected(sized.error());
    }
    if (Status sized = tile.material.resize(kTileTexelCount); !sized) {
        return make_unexpected(sized.error());
    }
    if (Status sized = tile.biome.resize(kTileTexelCount); !sized) {
        return make_unexpected(sized.error());
    }
    if (Status sized = tile.holes.resize(((kTileQuads * kTileQuads) + 63) / 64); !sized) {
        return make_unexpected(sized.error());
    }

    const u16 stored = quantise_height(layout, height);
    for (u16& sample : tile.heights) {
        sample = stored;
    }
    for (usize index = 0; index < tile.material.size(); ++index) {
        tile.material[index] = MaterialTexel{{0, 0, 0, 0}, {255, 0, 0, 0}};
        tile.biome[index] = 0;
    }
    for (u64& word : tile.holes) {
        word = 0;
    }
    tile.min_height = dequantise_height(layout, stored);
    tile.max_height = tile.min_height;
    return tile;
}

bool TerrainTile::hole(u32 qi, u32 qj) const noexcept {
    const u32 bit = (qj * kTileQuads) + qi;
    return (holes[bit / 64] & (u64{1} << (bit % 64))) != 0;
}

void TerrainTile::set_hole(u32 qi, u32 qj, bool value) noexcept {
    const u32 bit = (qj * kTileQuads) + qi;
    const u64 mask = u64{1} << (bit % 64);
    if (value) {
        holes[bit / 64] |= mask;
    } else {
        holes[bit / 64] &= ~mask;
    }
}

void TerrainTile::refresh_extent(const TileLayout& layout) noexcept {
    u16 low = 0xFFFFU;
    u16 high = 0;
    for (const u16 sample : heights.span()) {
        low = (sample < low) ? sample : low;
        high = (sample > high) ? sample : high;
    }
    min_height = dequantise_height(layout, low);
    max_height = dequantise_height(layout, high);
}

u64 TerrainTile::bytes() const noexcept {
    return static_cast<u64>(heights.size() * sizeof(u16)) +
           static_cast<u64>(material.size() * sizeof(MaterialTexel)) +
           static_cast<u64>(biome.size()) + static_cast<u64>(holes.size() * sizeof(u64));
}

f32 tile_height(const TileLayout& layout, const TerrainTile& tile, u32 i, u32 j) noexcept {
    return dequantise_height(layout, tile.stored(i, j));
}

Expected<TerrainTile, Error> derive_coarse(Allocator& allocator, const TileLayout& layout,
                                           const TileCoord& coarse,
                                           Span<const TerrainTile* const> children) noexcept {
    const u32 ratio = layout.level_ratio;
    if (children.size() != static_cast<usize>(ratio) * ratio) {
        return fail(ErrorCode::InvalidArgument,
                    "terrain: derive_coarse wants level_ratio^2 children, row-major");
    }
    if (coarse.level == 0) {
        return fail(ErrorCode::InvalidArgument, "terrain: level 0 is derived from nothing");
    }

    Expected<TerrainTile, Error> made =
        TerrainTile::create(allocator, layout, coarse, layout.height_min);
    if (!made) {
        return made;
    }
    TerrainTile tile = std::move(made.value());
    tile.derivation_key = 0;

    // DECIMATION, not averaging: sample (i, j) of the coarse tile is the same STORED u16 as the
    // fine sample standing at the same place in the world, so the two levels agree exactly along
    // every shared line and a level transition has no seam to close. See tile.h's header note.
    for (u32 j = 0; j < kTileVerts; ++j) {
        for (u32 i = 0; i < kTileVerts; ++i) {
            const u32 fine_i = i * ratio;
            const u32 fine_j = j * ratio;
            // The last line of the coarse tile belongs to the last child's last line.
            const u32 child_i = (fine_i == kTileQuads * ratio) ? ratio - 1 : fine_i / kTileQuads;
            const u32 child_j = (fine_j == kTileQuads * ratio) ? ratio - 1 : fine_j / kTileQuads;
            const TerrainTile* child = children[(child_j * ratio) + child_i];
            if (child == nullptr) {
                continue;
            }
            tile.set_stored(
                i, j,
                child->stored(fine_i - (child_i * kTileQuads), fine_j - (child_j * kTileQuads)));
        }
    }

    // Material, biome and holes decimate the same way, and a hole survives coarsening: a coarse
    // quad is a hole when the fine quad at its own corner is one, so a cave entrance does not
    // acquire an invisible floor at distance.
    for (u32 j = 0; j < kTileTexels; ++j) {
        for (u32 i = 0; i < kTileTexels; ++i) {
            const u32 fine_i = i * ratio;
            const u32 fine_j = j * ratio;
            const u32 child_i = fine_i / kTileTexels;
            const u32 child_j = fine_j / kTileTexels;
            const TerrainTile* child = children[(child_j * ratio) + child_i];
            if (child == nullptr) {
                continue;
            }
            const u32 local_i = fine_i - (child_i * kTileTexels);
            const u32 local_j = fine_j - (child_j * kTileTexels);
            tile.texel(i, j) = child->texel(local_i, local_j);
            tile.biome[(j * kTileTexels) + i] = child->biome[(local_j * kTileTexels) + local_i];
            tile.set_hole(i, j, child->hole(local_i, local_j));
        }
    }
    tile.refresh_extent(layout);
    return tile;
}

// --- The store
// ------------------------------------------------------------------------------------

TerrainStore::TerrainStore(Allocator& allocator, const TileLayout& layout) noexcept
    : allocator_(&allocator),
      layout_(layout),
      tiles_(allocator),
      index_(allocator),
      wanted_(allocator) {}

usize* TerrainStore::slot_of(const TileCoord& coord) noexcept {
    usize* slot = index_.find(tile_key(coord));
    if (slot == nullptr || !(tiles_[*slot].coord == coord)) {
        return nullptr;
    }
    return slot;
}

const usize* TerrainStore::slot_of(const TileCoord& coord) const noexcept {
    const usize* slot = index_.find(tile_key(coord));
    if (slot == nullptr || !(tiles_[*slot].coord == coord)) {
        return nullptr;
    }
    return slot;
}

Status TerrainStore::insert(TerrainTile&& tile) noexcept {
    if (tile.coord.level >= layout_.levels) {
        return fail(ErrorCode::OutOfRange, "terrain: that level is not declared by the layout");
    }
    if (tile.heights.size() != kTileHeights) {
        return fail(ErrorCode::InvalidArgument,
                    "terrain: a tile's height channel is the wrong size");
    }
    const TileCoord coord = tile.coord;
    if (usize* existing = slot_of(coord); existing != nullptr) {
        bytes_ -= tiles_[*existing].bytes();
        bytes_ += tile.bytes();
        tiles_[*existing] = std::move(tile);
        return ok();
    }

    const u64 added = tile.bytes();
    if (Status pushed = tiles_.push_back(std::move(tile)); !pushed) {
        return pushed;
    }
    Expected<usize*, Error> mapped = index_.insert(tile_key(coord), tiles_.size() - 1);
    if (!mapped) {
        tiles_.remove_unordered(tiles_.size() - 1);
        return make_unexpected(mapped.error());
    }
    bytes_ += added;
    return ok();
}

Status TerrainStore::evict(const TileCoord& coord) noexcept {
    usize* slot = slot_of(coord);
    if (slot == nullptr) {
        return fail(ErrorCode::NotFound, "terrain: that tile is not resident");
    }
    const usize victim = *slot;
    bytes_ -= tiles_[victim].bytes();
    (void)index_.remove(tile_key(coord));

    const usize last = tiles_.size() - 1;
    if (victim != last) {
        const TileCoord moved = tiles_[last].coord;
        tiles_[victim] = std::move(tiles_[last]);
        if (usize* moved_slot = index_.find(tile_key(moved)); moved_slot != nullptr) {
            *moved_slot = victim;
        }
    }
    tiles_.remove_unordered(last);
    return ok();
}

const TerrainTile* TerrainStore::find(const TileCoord& coord) const noexcept {
    const usize* slot = slot_of(coord);
    return (slot == nullptr) ? nullptr : &tiles_[*slot];
}

bool TerrainStore::is_resident(const TileCoord& coord) const noexcept {
    return slot_of(coord) != nullptr;
}

const TerrainTile* TerrainStore::finest_at(f64 x, f64 z, u8 from_level) const noexcept {
    for (u8 level = from_level; level < layout_.levels; ++level) {
        if (const TerrainTile* tile = find(tile_at(layout_, x, z, level)); tile != nullptr) {
            return tile;
        }
    }
    return nullptr;
}

Status TerrainStore::note_wanted(const TileCoord& coord, DetailReason reason) noexcept {
    Expected<u8*, Error> noted = wanted_.insert(tile_key(coord), static_cast<u8>(reason));
    if (!noted) {
        return make_unexpected(noted.error());
    }
    return ok();
}

void TerrainStore::clear_wanted() noexcept {
    wanted_.clear();
}

TileDetailReport TerrainStore::explain(f64 x, f64 z, u8 wanted_level) const noexcept {
    TileDetailReport report;
    report.level_wanted = wanted_level;
    for (u8 level = wanted_level; level < layout_.levels; ++level) {
        const TileCoord coord = tile_at(layout_, x, z, level);
        if (const TerrainTile* tile = find(coord); tile != nullptr) {
            report.used = coord;
            report.resolved = true;
            report.sample_metres = layout_.sample_metres(level);
            report.bytes = tile->bytes();
            // The cause is about the level that is MISSING, not the one that answered: a viewer
            // asking "why is this coarse" is asking about the finer tile it did not get.
            report.reason = DetailReason::Resident;
            if (level != wanted_level) {
                const TileCoord missing = tile_at(layout_, x, z, wanted_level);
                const u8* noted = wanted_.find(tile_key(missing));
                report.reason =
                    (noted == nullptr) ? DetailReason::Distance : static_cast<DetailReason>(*noted);
            }
            return report;
        }
    }
    report.resolved = false;
    const u8* noted = wanted_.find(tile_key(tile_at(layout_, x, z, wanted_level)));
    report.reason = (noted == nullptr) ? DetailReason::Distance : static_cast<DetailReason>(*noted);
    return report;
}

Status TerrainStore::tiles_at_level(u8 level, Array<TileCoord>& out) const noexcept {
    for (const TerrainTile& tile : tiles_.span()) {
        if (tile.coord.level != level) {
            continue;
        }
        if (Status pushed = out.push_back(tile.coord); !pushed) {
            return pushed;
        }
    }
    // Sorted, so a cook manifest, a diagnostic and a test all see one canonical order whatever the
    // insertion order was. Identity ordering rather than coordinate ordering, because identity is
    // what every other record in this engine keys on.
    Span<TileCoord> span = out.span();
    for (usize a = 1; a < span.size(); ++a) {
        TileCoord key = span[a];
        const TileId key_id = tile_id_of(layout_, key);
        usize b = a;
        while (b > 0 && tile_id_of(layout_, span[b - 1]).value > key_id.value) {
            span[b] = span[b - 1];
            --b;
        }
        span[b] = key;
    }
    return ok();
}

TerrainDiagnostics TerrainStore::diagnostics() const noexcept {
    TerrainDiagnostics diagnostics;
    diagnostics.bytes = bytes_;
    bool first = true;
    for (const TerrainTile& tile : tiles_.span()) {
        if (tile.coord.level < kMaxTerrainLevels) {
            ++diagnostics.tiles[tile.coord.level];
        }
        for (const u64 word : tile.holes.span()) {
            diagnostics.hole_quads += static_cast<u32>(std::popcount(word));
        }
        if (tile.coord.level != 0) {
            continue;
        }
        if (first) {
            diagnostics.min_tile_x = tile.coord.x;
            diagnostics.max_tile_x = tile.coord.x;
            diagnostics.min_tile_z = tile.coord.z;
            diagnostics.max_tile_z = tile.coord.z;
            first = false;
            continue;
        }
        diagnostics.min_tile_x =
            (tile.coord.x < diagnostics.min_tile_x) ? tile.coord.x : diagnostics.min_tile_x;
        diagnostics.max_tile_x =
            (tile.coord.x > diagnostics.max_tile_x) ? tile.coord.x : diagnostics.max_tile_x;
        diagnostics.min_tile_z =
            (tile.coord.z < diagnostics.min_tile_z) ? tile.coord.z : diagnostics.min_tile_z;
        diagnostics.max_tile_z =
            (tile.coord.z > diagnostics.max_tile_z) ? tile.coord.z : diagnostics.max_tile_z;
    }
    for (const u32 tile : diagnostics.tiles) {
        if (tile != 0) {
            ++diagnostics.resident_levels;
        }
    }
    return diagnostics;
}

Status cell_tile_footprint(const world::PartitionConfig& partition, const world::CellCoord& cell,
                           const TileLayout& layout, u8 level, Array<TileCoord>& out) noexcept {
    if (!partition.is_valid() || !layout.is_valid() || level >= layout.levels) {
        return fail(ErrorCode::InvalidArgument,
                    "terrain: the partition, the layout or the level does not name a footprint");
    }
    const Aabb bounds = world::cell_bounds(partition, cell);
    const TileCoord low =
        tile_at(layout, static_cast<f64>(bounds.min.x), static_cast<f64>(bounds.min.z), level);
    const TileCoord high =
        tile_at(layout, static_cast<f64>(bounds.max.x), static_cast<f64>(bounds.max.z), level);
    for (i32 z = low.z; z <= high.z; ++z) {
        for (i32 x = low.x; x <= high.x; ++x) {
            if (Status pushed = out.push_back(TileCoord{x, z, level}); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

u8 level_for_detail(const TileLayout& layout, f64 distance_metres,
                    f32 metres_per_unit_at_one_metre) noexcept {
    // The finest level whose sample spacing is still below the error the viewer tolerates at this
    // distance. The same shape as `virtual-geometry`'s screen-space error test: the tolerance grows
    // with distance, and the representation coarsens when it can do so under the tolerance.
    const f64 tolerance = static_cast<f64>(metres_per_unit_at_one_metre) *
                          (distance_metres < 1.0 ? 1.0 : distance_metres);
    u8 chosen = 0;
    for (u8 level = 0; level < layout.levels; ++level) {
        if (static_cast<f64>(layout.sample_metres(level)) <= tolerance) {
            chosen = level;
        }
    }
    return chosen;
}

}  // namespace cy::terrain
