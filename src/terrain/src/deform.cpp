// The delta over cooked terrain: stamping, the class-dependent invalidation, the overlay round trip
// and the bounded replication message. M10 task 2.2.

#include <cy/terrain/deform.h>

#include <cmath>
#include <cstring>

namespace cy::terrain {
namespace {

/// Floor division for a positive divisor; see tile.cpp for why truncation is wrong here.
[[nodiscard]] i64 floor_div(i64 value, i64 divisor) noexcept {
    const i64 quotient = value / divisor;
    return ((value % divisor != 0) && ((value < 0) != (divisor < 0))) ? quotient - 1 : quotient;
}

[[nodiscard]] u64 delta_key(const TileCoord& coord) noexcept {
    u64 value = hash_integer(static_cast<u64>(static_cast<u32>(coord.x)), kTerrainHashSeed);
    value = hash_combine(value, static_cast<u64>(static_cast<u32>(coord.z)));
    return hash_combine(value, coord.level);
}

/// The blob one tile's delta encodes to. Fixed layout, little-endian by memcpy of the host's own
/// representation — the same discipline `world::PersistenceOverlay` uses for a component override,
/// which `save-and-persistence` is what converts to a portable form when it encodes the overlay.
struct DeltaHeader {
    i32 x = 0;
    i32 z = 0;
    u8 level = 0;
    u8 reserved[3] = {};
    u32 gameplay_count = 0;
    u32 visual_count = 0;
    u32 hole_words = 0;
};

/// One changed sample, as the message carries it. An index and a value, so a crater in a large
/// world sends the crater rather than the tile.
struct DeltaSample {
    u32 index = 0;
    f32 value = 0.0F;
};

template <typename T>
[[nodiscard]] Status append_bytes(Array<u8>& out, const T& value) noexcept {
    const auto* bytes = reinterpret_cast<const u8*>(&value);
    return out.append(Span<const u8>(bytes, sizeof(T)));
}

template <typename T>
[[nodiscard]] bool take_bytes(Span<const u8>& cursor, T& out) noexcept {
    if (cursor.size() < sizeof(T)) {
        return false;
    }
    std::memcpy(&out, cursor.data(), sizeof(T));
    cursor = cursor.subspan(sizeof(T));
    return true;
}

}  // namespace

const char* deformation_class_name(DeformationClass klass) noexcept {
    switch (klass) {
        case DeformationClass::Visual:
            return "visual";
        case DeformationClass::Gameplay:
            return "gameplay";
        case DeformationClass::Structural:
            return "structural";
    }
    return "unknown";
}

f32 stamp_weight(const Deformation& deformation, f64 x, f64 z) noexcept {
    const TerrainBounds& bounds = deformation.bounds;
    if (!bounds.contains(x, z)) {
        return 0.0F;
    }
    if (deformation.shape == DeformationShape::Box) {
        return 1.0F;
    }
    const f64 centre_x = (bounds.min_x + bounds.max_x) * 0.5;
    const f64 centre_z = (bounds.min_z + bounds.max_z) * 0.5;
    const f64 radius_x = (bounds.max_x - bounds.min_x) * 0.5;
    const f64 radius_z = (bounds.max_z - bounds.min_z) * 0.5;
    if (radius_x <= 0.0 || radius_z <= 0.0) {
        return 0.0F;
    }
    const f64 dx = (x - centre_x) / radius_x;
    const f64 dz = (z - centre_z) / radius_z;
    const f64 distance = std::sqrt((dx * dx) + (dz * dz));
    if (distance >= 1.0) {
        return 0.0F;
    }
    // Smooth to zero at the rim, so a crater's edge is not a cliff one sample wide that navigation
    // then has to reject as unwalkable.
    const f32 t = static_cast<f32>(1.0 - distance);
    return t * t * (3.0F - (2.0F * t));
}

TerrainDeltaStore::TerrainDeltaStore(Allocator& allocator, const TileLayout& layout) noexcept
    : allocator_(&allocator), layout_(layout), tiles_(allocator), index_(allocator) {}

TerrainDeltaStore::TileDelta* TerrainDeltaStore::find_delta(const TileCoord& coord) noexcept {
    usize* slot = index_.find(delta_key(coord));
    if (slot == nullptr || !(tiles_[*slot].coord == coord)) {
        return nullptr;
    }
    return &tiles_[*slot];
}

const TerrainDeltaStore::TileDelta* TerrainDeltaStore::find_delta(
    const TileCoord& coord) const noexcept {
    const usize* slot = index_.find(delta_key(coord));
    if (slot == nullptr || !(tiles_[*slot].coord == coord)) {
        return nullptr;
    }
    return &tiles_[*slot];
}

Expected<TerrainDeltaStore::TileDelta*, Error> TerrainDeltaStore::delta_for(
    const TileCoord& coord) noexcept {
    if (TileDelta* existing = find_delta(coord); existing != nullptr) {
        return existing;
    }
    TileDelta delta(*allocator_);
    delta.coord = coord;
    if (Status sized = delta.gameplay.resize(kTileHeights); !sized) {
        return make_unexpected(sized.error());
    }
    if (Status sized = delta.visual.resize(kTileHeights); !sized) {
        return make_unexpected(sized.error());
    }
    if (Status sized = delta.holes.resize(((kTileQuads * kTileQuads) + 63) / 64); !sized) {
        return make_unexpected(sized.error());
    }
    for (usize index = 0; index < kTileHeights; ++index) {
        delta.gameplay[index] = 0.0F;
        delta.visual[index] = 0.0F;
    }
    for (u64& word : delta.holes) {
        word = 0;
    }
    if (Status pushed = tiles_.push_back(std::move(delta)); !pushed) {
        return make_unexpected(pushed.error());
    }
    Expected<usize*, Error> mapped = index_.insert(delta_key(coord), tiles_.size() - 1);
    if (!mapped) {
        tiles_.remove_unordered(tiles_.size() - 1);
        return make_unexpected(mapped.error());
    }
    return &tiles_[tiles_.size() - 1];
}

TerrainDeltaStore::FineSample TerrainDeltaStore::fine_sample_of(const TileCoord& coord, u32 i,
                                                                u32 j) const noexcept {
    // The absolute LEVEL-0 lattice index of the sample, which is where the delta for it lives. A
    // coarse tile's sample stands at the same world position as one of the fine lattice's, and this
    // is that correspondence written once. See deform.h's header note.
    i64 scale = 1;
    for (u8 level = 0; level < coord.level; ++level) {
        scale *= static_cast<i64>(layout_.level_ratio);
    }
    const i64 gi = ((static_cast<i64>(coord.x) * kTileQuads) + static_cast<i64>(i)) * scale;
    const i64 gj = ((static_cast<i64>(coord.z) * kTileQuads) + static_cast<i64>(j)) * scale;
    FineSample sample;
    sample.coord = TileCoord{static_cast<i32>(floor_div(gi, kTileQuads)),
                             static_cast<i32>(floor_div(gj, kTileQuads)), 0};
    sample.i = static_cast<u32>(gi - (static_cast<i64>(sample.coord.x) * kTileQuads));
    sample.j = static_cast<u32>(gj - (static_cast<i64>(sample.coord.z) * kTileQuads));
    return sample;
}

f32 TerrainDeltaStore::height_delta(const TileCoord& coord, u32 i, u32 j) const noexcept {
    if (i >= kTileVerts || j >= kTileVerts) {
        return 0.0F;
    }
    const FineSample fine = fine_sample_of(coord, i, j);
    const TileDelta* delta = find_delta(fine.coord);
    return (delta == nullptr) ? 0.0F : delta->gameplay[(fine.j * kTileVerts) + fine.i];
}

f32 TerrainDeltaStore::visual_delta(const TileCoord& coord, u32 i, u32 j) const noexcept {
    if (i >= kTileVerts || j >= kTileVerts) {
        return 0.0F;
    }
    const FineSample fine = fine_sample_of(coord, i, j);
    const TileDelta* delta = find_delta(fine.coord);
    return (delta == nullptr) ? 0.0F : delta->visual[(fine.j * kTileVerts) + fine.i];
}

bool TerrainDeltaStore::hole_delta(const TileCoord& coord, u32 qi, u32 qj) const noexcept {
    if (qi >= kTileQuads || qj >= kTileQuads) {
        return false;
    }
    const FineSample fine = fine_sample_of(coord, qi, qj);
    const TileDelta* delta = find_delta(fine.coord);
    if (delta == nullptr || fine.i >= kTileQuads || fine.j >= kTileQuads) {
        return false;
    }
    const u32 bit = (fine.j * kTileQuads) + fine.i;
    return (delta->holes[bit / 64] & (u64{1} << (bit % 64))) != 0;
}

bool TerrainDeltaStore::has_delta(const TileCoord& coord) const noexcept {
    return find_delta(coord) != nullptr;
}

void TerrainDeltaStore::clear() noexcept {
    tiles_.clear();
    index_.clear();
}

Expected<bool, Error> TerrainDeltaStore::stamp_tile(const Deformation& deformation,
                                                    const TileCoord& coord) noexcept {
    Expected<TileDelta*, Error> found = delta_for(coord);
    if (!found) {
        return make_unexpected(found.error());
    }
    TileDelta& delta = *found.value();
    const bool visual = deformation.klass == DeformationClass::Visual;
    bool moved = false;
    for (u32 j = 0; j < kTileVerts; ++j) {
        for (u32 i = 0; i < kTileVerts; ++i) {
            const TerrainPoint at = sample_position(layout_, coord, i, j);
            const f32 weight = stamp_weight(deformation, at.x, at.z);
            if (weight <= 0.0F) {
                continue;
            }
            const f32 change = deformation.amount * weight;
            // Two channels, never one. The header note says why.
            if (visual) {
                delta.visual[(j * kTileVerts) + i] += change;
            } else {
                delta.gameplay[(j * kTileVerts) + i] += change;
            }
            moved = true;
        }
    }
    return moved;
}

Expected<InvalidationSet, Error> TerrainDeltaStore::apply(const Deformation& deformation) noexcept {
    InvalidationSet set(*allocator_);
    if (deformation.klass == DeformationClass::Structural) {
        // The refusal, naming what is missing. See the header note.
        return fail(ErrorCode::NotImplemented,
                    "terrain: a structural deformation needs the SignedDistanceField "
                    "representation, which this terrain does not carry");
    }
    if (deformation.opens_hole) {
        return fail(ErrorCode::NotImplemented,
                    "terrain: opening a hole at run time is a structural edit and needs the "
                    "SignedDistanceField representation");
    }
    if (deformation.bounds.max_x <= deformation.bounds.min_x ||
        deformation.bounds.max_z <= deformation.bounds.min_z) {
        return fail(ErrorCode::InvalidArgument, "terrain: a deformation needs a positive extent");
    }

    // Level 0 only. Every coarser level reads the same delta through `fine_sample_of()`, so a
    // query answered from a macro tile sees the same pit — without a second copy of it to keep in
    // step and without a replicated message carrying a coarse tile that spans the whole map.
    const TileCoord low = tile_at(layout_, deformation.bounds.min_x, deformation.bounds.min_z, 0);
    const TileCoord high = tile_at(layout_, deformation.bounds.max_x, deformation.bounds.max_z, 0);
    for (i32 z = low.z; z <= high.z; ++z) {
        for (i32 x = low.x; x <= high.x; ++x) {
            const TileCoord coord{x, z, 0};
            Expected<bool, Error> stamped = stamp_tile(deformation, coord);
            if (!stamped) {
                return make_unexpected(stamped.error());
            }
            if (!stamped.value()) {
                continue;
            }
            if (Status pushed = set.tiles.push_back(coord); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
    }

    // The invalidation the class costs, and nothing beyond it. `terrain`'s own table.
    set.rendering = true;
    if (deformation.klass != DeformationClass::Visual) {
        set.collision = true;
        set.navigation = true;
        if (Status pushed = set.navigation_dirty.push_back(deformation.bounds); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    return set;
}

// --- Persistence
// ----------------------------------------------------------------------------------

Status TerrainDeltaStore::encode_tile(const TileDelta& delta, Array<u8>& out) noexcept {
    DeltaHeader header;
    header.x = delta.coord.x;
    header.z = delta.coord.z;
    header.level = delta.coord.level;
    header.hole_words = static_cast<u32>(delta.holes.size());
    for (usize index = 0; index < kTileHeights; ++index) {
        header.gameplay_count += (delta.gameplay[index] != 0.0F) ? 1U : 0U;
        header.visual_count += (delta.visual[index] != 0.0F) ? 1U : 0U;
    }
    if (Status written = append_bytes(out, header); !written) {
        return written;
    }
    for (usize index = 0; index < kTileHeights; ++index) {
        if (delta.gameplay[index] == 0.0F) {
            continue;
        }
        const DeltaSample sample{static_cast<u32>(index), delta.gameplay[index]};
        if (Status written = append_bytes(out, sample); !written) {
            return written;
        }
    }
    for (usize index = 0; index < kTileHeights; ++index) {
        if (delta.visual[index] == 0.0F) {
            continue;
        }
        const DeltaSample sample{static_cast<u32>(index), delta.visual[index]};
        if (Status written = append_bytes(out, sample); !written) {
            return written;
        }
    }
    for (const u64 word : delta.holes.span()) {
        if (Status written = append_bytes(out, word); !written) {
            return written;
        }
    }
    return ok();
}

Status TerrainDeltaStore::record_into(world::PersistenceOverlay& overlay,
                                      const world::PartitionConfig& partition) const noexcept {
    Array<u8> blob(*allocator_);
    for (const TileDelta& delta : tiles_.span()) {
        blob.clear();
        if (Status encoded = encode_tile(delta, blob); !encoded) {
            return encoded;
        }
        // Keyed by the cell holding the tile's centre, so a save of a world that is mostly unloaded
        // still finds every delta: the overlay is keyed by cell and answers for cells that have
        // never been resident.
        const TerrainBounds bounds = tile_bounds(layout_, delta.coord);
        const world::WorldVec3d centre{(bounds.min_x + bounds.max_x) * 0.5, 0.0,
                                       (bounds.min_z + bounds.max_z) * 0.5};
        const world::WorldPosition position = world::from_absolute(partition, centre, 0);
        const world::CellId cell = world::cell_id_of(partition, position.cell);
        if (Status recorded = overlay.record_blob(
                cell, kOverlayChannelTerrain, tile_id_of(layout_, delta.coord).value, blob.span());
            !recorded) {
            return recorded;
        }
    }
    return ok();
}

Status TerrainDeltaStore::restore_from(const world::PersistenceOverlay& overlay,
                                       const world::PartitionConfig& partition) noexcept {
    (void)partition;
    clear();
    Array<world::CellId> cells(*allocator_);
    if (Status listed = overlay.cells(cells); !listed) {
        return listed;
    }
    for (const world::CellId cell : cells.span()) {
        const world::CellOverlay* entry = overlay.find(cell);
        if (entry == nullptr) {
            continue;
        }
        for (const world::OverlayBlob& record : entry->blobs.span()) {
            if (record.channel != kOverlayChannelTerrain) {
                continue;
            }
            const Span<const u8> bytes = overlay.blob(cell, record.channel, record.key);
            Expected<u32, Error> applied = apply_encoded(bytes);
            if (!applied) {
                return make_unexpected(applied.error());
            }
        }
    }
    return ok();
}

Status TerrainDeltaStore::encode_region(const TerrainBounds& bounds,
                                        Array<u8>& out) const noexcept {
    for (const TileDelta& delta : tiles_.span()) {
        if (!tile_bounds(layout_, delta.coord).overlaps(bounds)) {
            continue;
        }
        if (Status encoded = encode_tile(delta, out); !encoded) {
            return encoded;
        }
    }
    return ok();
}

Expected<u32, Error> TerrainDeltaStore::apply_encoded(Span<const u8> message) noexcept {
    u32 tiles = 0;
    Span<const u8> cursor = message;
    while (!cursor.empty()) {
        DeltaHeader header;
        if (!take_bytes(cursor, header)) {
            return fail(ErrorCode::InvalidArgument, "terrain: a delta message ended mid-header");
        }
        const TileCoord coord{header.x, header.z, header.level};
        Expected<TileDelta*, Error> found = delta_for(coord);
        if (!found) {
            return make_unexpected(found.error());
        }
        TileDelta& delta = *found.value();

        for (u32 index = 0; index < header.gameplay_count; ++index) {
            DeltaSample sample;
            if (!take_bytes(cursor, sample) || sample.index >= kTileHeights) {
                return fail(ErrorCode::InvalidArgument,
                            "terrain: a delta message named a sample outside its tile");
            }
            delta.gameplay[sample.index] = sample.value;
        }
        for (u32 index = 0; index < header.visual_count; ++index) {
            DeltaSample sample;
            if (!take_bytes(cursor, sample) || sample.index >= kTileHeights) {
                return fail(ErrorCode::InvalidArgument,
                            "terrain: a delta message named a sample outside its tile");
            }
            delta.visual[sample.index] = sample.value;
        }
        if (header.hole_words != delta.holes.size()) {
            return fail(ErrorCode::InvalidArgument,
                        "terrain: a delta message carries a different hole bitmap size");
        }
        for (u32 index = 0; index < header.hole_words; ++index) {
            u64 word = 0;
            if (!take_bytes(cursor, word)) {
                return fail(ErrorCode::InvalidArgument, "terrain: a delta message ended mid-hole");
            }
            delta.holes[index] = word;
        }
        ++tiles;
    }
    return tiles;
}

}  // namespace cy::terrain
