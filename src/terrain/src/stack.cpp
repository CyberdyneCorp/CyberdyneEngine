// The modifier stack: the generator, the fold, the declared halo, the derivation key and the cook
// that flattens it. M10 task 2.2.

#include <cy/terrain/stack.h>

#include <bit>
#include <cmath>

namespace cy::terrain {
namespace {

[[nodiscard]] f32 clamp_f32(f32 value, f32 low, f32 high) noexcept {
    if (value < low) {
        return low;
    }
    return (value > high) ? high : value;
}

[[nodiscard]] i64 ifloor(f64 value) noexcept {
    return static_cast<i64>(std::floor(value));
}

[[nodiscard]] f32 smooth(f32 t) noexcept {
    return t * t * (3.0F - (2.0F * t));
}

/// The stream one modifier draws from. `substream` keyed by the modifier's INDEX IN THE STACK —
/// a stable identifier of the modifier — then `draw` keyed by the lattice point. The M10 spike's
/// fourth condition, applied to terrain: never a traversal counter.
[[nodiscard]] determinism::RandomStream modifier_stream(u64 seed, u64 index) noexcept {
    const determinism::StreamId root = determinism::stream_id("terrain.stack.modifier");
    return {seed, determinism::substream(root, index), determinism::StreamPurpose::Authoritative};
}

[[nodiscard]] f32 lattice(const determinism::RandomStream& stream, i64 x, i64 z,
                          u64 octave) noexcept {
    const u64 packed = (static_cast<u64>(static_cast<u32>(static_cast<i32>(x))) << 32U) |
                       static_cast<u64>(static_cast<u32>(static_cast<i32>(z)));
    return stream.unit_float(determinism::SimulationPoint{}, packed, octave);
}

/// Value noise at one position, for one octave.
[[nodiscard]] f32 value_noise(const determinism::RandomStream& stream, f64 x, f64 z, f64 period,
                              u64 octave) noexcept {
    const f64 lx = x / period;
    const f64 lz = z / period;
    const i64 x0 = ifloor(lx);
    const i64 z0 = ifloor(lz);
    const f32 fx = smooth(static_cast<f32>(lx - static_cast<f64>(x0)));
    const f32 fz = smooth(static_cast<f32>(lz - static_cast<f64>(z0)));
    const f32 v00 = lattice(stream, x0, z0, octave);
    const f32 v10 = lattice(stream, x0 + 1, z0, octave);
    const f32 v01 = lattice(stream, x0, z0 + 1, octave);
    const f32 v11 = lattice(stream, x0 + 1, z0 + 1, octave);
    return (((v00 * (1.0F - fx)) + (v10 * fx)) * (1.0F - fz)) +
           (((v01 * (1.0F - fx)) + (v11 * fx)) * fz);
}

/// Distance from a point to a polyline, and the parameter of the closest point along it.
struct SplineHit {
    f64 distance = 0.0;
    f32 along = 0.0F;  ///< 0 at the first point, 1 at the last
    bool valid = false;
};

[[nodiscard]] SplineHit closest_on_spline(Span<const TerrainPoint> points, f64 x, f64 z) noexcept {
    SplineHit best;
    if (points.size() < 2) {
        return best;
    }
    f64 travelled = 0.0;
    f64 total = 0.0;
    for (usize index = 1; index < points.size(); ++index) {
        const f64 dx = points[index].x - points[index - 1].x;
        const f64 dz = points[index].z - points[index - 1].z;
        total += std::sqrt((dx * dx) + (dz * dz));
    }
    if (total <= 0.0) {
        return best;
    }
    for (usize index = 1; index < points.size(); ++index) {
        const TerrainPoint& a = points[index - 1];
        const TerrainPoint& b = points[index];
        const f64 dx = b.x - a.x;
        const f64 dz = b.z - a.z;
        const f64 length_squared = (dx * dx) + (dz * dz);
        f64 t = 0.0;
        if (length_squared > 0.0) {
            t = (((x - a.x) * dx) + ((z - a.z) * dz)) / length_squared;
            t = (t < 0.0) ? 0.0 : t;
            t = (t > 1.0) ? 1.0 : t;
        }
        const f64 px = a.x + (dx * t);
        const f64 pz = a.z + (dz * t);
        const f64 distance = std::sqrt(((x - px) * (x - px)) + ((z - pz) * (z - pz)));
        const f64 segment = std::sqrt(length_squared);
        if (!best.valid || distance < best.distance) {
            best.valid = true;
            best.distance = distance;
            best.along = static_cast<f32>((travelled + (segment * t)) / total);
        }
        travelled += segment;
    }
    return best;
}

}  // namespace

const char* modifier_kind_name(ModifierKind kind) noexcept {
    switch (kind) {
        case ModifierKind::Noise:
            return "noise";
        case ModifierKind::Erosion:
            return "erosion";
        case ModifierKind::RoadSpline:
            return "road-spline";
        case ModifierKind::RiverCarve:
            return "river-carve";
        case ModifierKind::Flatten:
            return "flatten";
        case ModifierKind::Crater:
            return "crater";
        case ModifierKind::Sculpt:
            return "sculpt";
        case ModifierKind::Hole:
            return "hole";
    }
    return "unknown";
}

TerrainBounds modifier_reach(const Modifier& modifier) noexcept {
    return modifier.bounds.expanded(static_cast<f64>(modifier.radius));
}

ModifierStack::ModifierStack(Allocator& allocator, const TileLayout& layout, u64 seed) noexcept
    : allocator_(&allocator),
      layout_(layout),
      seed_(seed),
      modifiers_(allocator),
      points_(allocator),
      samples_(allocator),
      decorations_(allocator) {}

Expected<u32, Error> ModifierStack::add(const Modifier& modifier) noexcept {
    if (Status pushed = modifiers_.push_back(modifier); !pushed) {
        return make_unexpected(pushed.error());
    }
    return static_cast<u32>(modifiers_.size() - 1);
}

Status ModifierStack::insert_at(u32 index, const Modifier& modifier) noexcept {
    if (index > modifiers_.size()) {
        return fail(ErrorCode::OutOfRange, "terrain: that position is past the end of the stack");
    }
    if (Status pushed = modifiers_.push_back(modifier); !pushed) {
        return pushed;
    }
    Span<Modifier> span = modifiers_.span();
    for (usize slot = span.size() - 1; slot > index; --slot) {
        const Modifier moved = span[slot - 1];
        span[slot - 1] = span[slot];
        span[slot] = moved;
    }
    return ok();
}

Status ModifierStack::move(u32 from, u32 to) noexcept {
    if (from >= modifiers_.size() || to >= modifiers_.size()) {
        return fail(ErrorCode::OutOfRange, "terrain: no such modifier");
    }
    Span<Modifier> span = modifiers_.span();
    const Modifier moving = span[from];
    if (from < to) {
        for (u32 slot = from; slot < to; ++slot) {
            span[slot] = span[slot + 1];
        }
    } else {
        for (u32 slot = from; slot > to; --slot) {
            span[slot] = span[slot - 1];
        }
    }
    span[to] = moving;
    return ok();
}

Status ModifierStack::set_enabled(u32 index, bool enabled) noexcept {
    if (index >= modifiers_.size()) {
        return fail(ErrorCode::OutOfRange, "terrain: no such modifier");
    }
    modifiers_[index].enabled = enabled;
    return ok();
}

Status ModifierStack::edit(u32 index, const Modifier& modifier) noexcept {
    if (index >= modifiers_.size()) {
        return fail(ErrorCode::OutOfRange, "terrain: no such modifier");
    }
    // The pool ranges are the stack's bookkeeping, not the caller's: an edit that carried a stale
    // `first_point` would silently point a road at another road's spline.
    Modifier updated = modifier;
    updated.first_point = modifiers_[index].first_point;
    updated.point_count = modifiers_[index].point_count;
    updated.first_sample = modifiers_[index].first_sample;
    updated.stamp_edge = modifiers_[index].stamp_edge;
    modifiers_[index] = updated;
    return ok();
}

Status ModifierStack::add_spline(u32 index, Span<const TerrainPoint> points) noexcept {
    if (index >= modifiers_.size()) {
        return fail(ErrorCode::OutOfRange, "terrain: no such modifier");
    }
    const auto first = static_cast<u32>(points_.size());
    if (Status appended = points_.append(points); !appended) {
        return appended;
    }
    modifiers_[index].first_point = first;
    modifiers_[index].point_count = static_cast<u32>(points.size());
    return ok();
}

Status ModifierStack::add_sculpt(u32 index, u32 edge, Span<const f32> offsets) noexcept {
    if (index >= modifiers_.size()) {
        return fail(ErrorCode::OutOfRange, "terrain: no such modifier");
    }
    if (edge < 2 || offsets.size() != static_cast<usize>(edge) * edge) {
        return fail(ErrorCode::InvalidArgument,
                    "terrain: a sculpt stamp is edge x edge offsets over the modifier's bounds");
    }
    const auto first = static_cast<u32>(samples_.size());
    if (Status appended = samples_.append(offsets); !appended) {
        return appended;
    }
    modifiers_[index].first_sample = first;
    modifiers_[index].stamp_edge = edge;
    return ok();
}

Status ModifierStack::declare_decoration(u8 layer, bool suppresses) noexcept {
    for (u16& existing : decorations_.span()) {
        if (static_cast<u8>(existing & 0xFFU) == layer) {
            existing = static_cast<u16>(layer | (suppresses ? 0x100U : 0U));
            return ok();
        }
    }
    return decorations_.push_back(static_cast<u16>(layer | (suppresses ? 0x100U : 0U)));
}

bool ModifierStack::suppresses_foliage(u8 layer) const noexcept {
    for (const u16 entry : decorations_.span()) {
        if (static_cast<u8>(entry & 0xFFU) == layer) {
            return (entry & 0x100U) != 0;
        }
    }
    return false;
}

// --- Evaluation
// -----------------------------------------------------------------------------------

u32 ModifierStack::halo_samples(const TileCoord& coord) const noexcept {
    // The declared halo, in samples, and it is the MAXIMUM over the enabled modifiers rather than
    // the sum: each is evaluated over the same padded grid, so the widest one decides.
    const f32 spacing = layout_.sample_metres(coord.level);
    u32 pad = 0;
    for (const Modifier& modifier : modifiers_.span()) {
        if (!modifier.enabled) {
            continue;
        }
        u32 wanted = static_cast<u32>(std::ceil(modifier.radius / spacing));
        if (modifier.kind == ModifierKind::Erosion) {
            // An erosion pass reads one sample in every direction, so K passes need K samples of
            // halo to be exact. A halo smaller than this produces a tile whose interior depends on
            // where the tile boundary is — a seam, and the local form of the spike's second
            // condition.
            wanted = (wanted > modifier.iterations) ? wanted : modifier.iterations;
        }
        pad = (wanted > pad) ? wanted : pad;
    }
    return (pad > kTileQuads) ? kTileQuads : pad;
}

Expected<ModifierStack::Padded, Error> ModifierStack::generate(Allocator& allocator,
                                                               const TileCoord& coord,
                                                               u32 pad) const noexcept {
    Padded grid(allocator);
    grid.pad = pad;
    grid.edge = kTileVerts + (2 * pad);
    if (Status sized = grid.heights.resize(static_cast<usize>(grid.edge) * grid.edge); !sized) {
        return make_unexpected(sized.error());
    }

    const determinism::RandomStream stream = modifier_stream(seed_, 0);
    for (u32 j = 0; j < grid.edge; ++j) {
        for (u32 i = 0; i < grid.edge; ++i) {
            const i64 sample_i = static_cast<i64>(i) - static_cast<i64>(pad);
            const i64 sample_j = static_cast<i64>(j) - static_cast<i64>(pad);
            const f64 spacing = layout_.tile_size(coord.level) / static_cast<f64>(kTileQuads);
            const f64 x =
                layout_.origin_x +
                (static_cast<f64>((static_cast<i64>(coord.x) * kTileQuads) + sample_i) * spacing);
            const f64 z =
                layout_.origin_z +
                (static_cast<f64>((static_cast<i64>(coord.z) * kTileQuads) + sample_j) * spacing);

            f32 height = generator_.base_height;
            f64 period = static_cast<f64>(generator_.period);
            f32 amplitude = generator_.amplitude;
            for (u32 octave = 0; octave < generator_.octaves; ++octave) {
                height += (value_noise(stream, x, z, period, octave) - 0.5F) * 2.0F * amplitude;
                period *= 0.5;
                amplitude *= 0.5F;
            }
            grid.at(i, j) = height;
        }
    }
    return grid;
}

f32 ModifierStack::contribution(const Modifier& modifier, u32 index, f64 x, f64 z,
                                f32 current) const noexcept {
    const TerrainBounds reach = modifier_reach(modifier);
    if (!reach.contains(x, z)) {
        return current;
    }
    switch (modifier.kind) {
        case ModifierKind::Noise: {
            const determinism::RandomStream stream = modifier_stream(seed_, index + 1);
            f32 total = 0.0F;
            f64 period = static_cast<f64>(modifier.period);
            f32 amplitude = modifier.amplitude;
            for (u32 octave = 0; octave < modifier.iterations; ++octave) {
                total += (value_noise(stream, x, z, period, octave) - 0.5F) * 2.0F * amplitude;
                period *= 0.5;
                amplitude *= 0.5F;
            }
            return current + total;
        }
        case ModifierKind::Flatten: {
            if (!modifier.bounds.contains(x, z)) {
                return current;
            }
            return modifier.height;
        }
        case ModifierKind::Crater: {
            const f64 centre_x = (modifier.bounds.min_x + modifier.bounds.max_x) * 0.5;
            const f64 centre_z = (modifier.bounds.min_z + modifier.bounds.max_z) * 0.5;
            const f64 radius = (modifier.bounds.max_x - modifier.bounds.min_x) * 0.5;
            if (radius <= 0.0) {
                return current;
            }
            const f64 dx = (x - centre_x) / radius;
            const f64 dz = (z - centre_z) / radius;
            const f64 distance = std::sqrt((dx * dx) + (dz * dz));
            if (distance >= 1.0) {
                return current;
            }
            return current + (modifier.amplitude * smooth(static_cast<f32>(1.0 - distance)));
        }
        case ModifierKind::Sculpt: {
            if (modifier.stamp_edge < 2 || !modifier.bounds.contains(x, z)) {
                return current;
            }
            const f64 span_x = modifier.bounds.max_x - modifier.bounds.min_x;
            const f64 span_z = modifier.bounds.max_z - modifier.bounds.min_z;
            const auto edge = static_cast<f64>(modifier.stamp_edge - 1);
            const auto si = static_cast<u32>(
                clamp_f32(static_cast<f32>(((x - modifier.bounds.min_x) / span_x * edge) + 0.5),
                          0.0F, static_cast<f32>(edge)));
            const auto sj = static_cast<u32>(
                clamp_f32(static_cast<f32>(((z - modifier.bounds.min_z) / span_z * edge) + 0.5),
                          0.0F, static_cast<f32>(edge)));
            const usize offset =
                modifier.first_sample + (static_cast<usize>(sj) * modifier.stamp_edge) + si;
            if (offset >= samples_.size()) {
                return current;
            }
            // ADDED over whatever is beneath. That is what makes an erosion pass inserted below a
            // sculpt reapply the sculpt rather than erase it.
            return current + (samples_[offset] * modifier.amplitude);
        }
        case ModifierKind::RoadSpline:
        case ModifierKind::RiverCarve: {
            const Span<const TerrainPoint> points =
                points_.span().subspan(modifier.first_point, modifier.point_count);
            const SplineHit hit = closest_on_spline(points, x, z);
            const f64 half_width = static_cast<f64>(modifier.period) * 0.5;
            if (!hit.valid || half_width <= 0.0 || hit.distance > half_width) {
                return current;
            }
            const f32 weight =
                smooth(clamp_f32(static_cast<f32>(1.0 - (hit.distance / half_width)), 0.0F, 1.0F));
            const f32 target = (modifier.kind == ModifierKind::RoadSpline)
                                   ? modifier.height
                                   : current + modifier.height;
            return current + ((target - current) * weight);
        }
        case ModifierKind::Erosion:
        case ModifierKind::Hole:
            // Handled in `apply()`: one reads a neighbourhood and the other writes no height.
            return current;
    }
    return current;
}

void ModifierStack::apply(const Modifier& modifier, u32 index, const TileCoord& coord,
                          Padded& grid) const noexcept {
    if (modifier.kind == ModifierKind::Hole) {
        return;  // A hole changes no height. `evaluate()` marks the quads.
    }

    const f64 spacing = layout_.tile_size(coord.level) / static_cast<f64>(kTileQuads);
    if (modifier.kind == ModifierKind::Erosion) {
        // Every declared iteration, to convergence of the declared program rather than to a
        // budget — the spike's third condition. Each pass shrinks the usable region by one sample,
        // which is exactly what the declared halo pays for.
        for (u32 pass = 0; pass < modifier.iterations; ++pass) {
            for (u32 j = 1; j + 1 < grid.edge; ++j) {
                for (u32 i = 1; i + 1 < grid.edge; ++i) {
                    const f32 centre = grid.at(i, j);
                    const f32 average = (grid.at(i - 1, j) + grid.at(i + 1, j) + grid.at(i, j - 1) +
                                         grid.at(i, j + 1)) *
                                        0.25F;
                    grid.at(i, j) =
                        centre + ((average - centre) * clamp_f32(modifier.amplitude, 0.0F, 1.0F));
                }
            }
        }
        return;
    }

    for (u32 j = 0; j < grid.edge; ++j) {
        for (u32 i = 0; i < grid.edge; ++i) {
            const i64 sample_i = static_cast<i64>(i) - static_cast<i64>(grid.pad);
            const i64 sample_j = static_cast<i64>(j) - static_cast<i64>(grid.pad);
            const f64 x =
                layout_.origin_x +
                (static_cast<f64>((static_cast<i64>(coord.x) * kTileQuads) + sample_i) * spacing);
            const f64 z =
                layout_.origin_z +
                (static_cast<f64>((static_cast<i64>(coord.z) * kTileQuads) + sample_j) * spacing);
            grid.at(i, j) = contribution(modifier, index, x, z, grid.at(i, j));
        }
    }
}

void ModifierStack::write_material(const Modifier& modifier, const TileCoord& coord,
                                   TerrainTile& tile) const noexcept {
    for (u32 j = 0; j < kTileTexels; ++j) {
        for (u32 i = 0; i < kTileTexels; ++i) {
            const TerrainPoint at = sample_position(layout_, coord, i, j);
            if (modifier.kind == ModifierKind::Hole) {
                if (modifier.bounds.contains(at.x, at.z)) {
                    tile.set_hole(i, j, true);
                }
                continue;
            }
            if (modifier.kind != ModifierKind::RoadSpline &&
                modifier.kind != ModifierKind::RiverCarve) {
                continue;
            }
            const Span<const TerrainPoint> points =
                points_.span().subspan(modifier.first_point, modifier.point_count);
            const SplineHit hit = closest_on_spline(points, at.x, at.z);
            if (!hit.valid || hit.distance > static_cast<f64>(modifier.period) * 0.5) {
                continue;
            }
            // The decoration's own layer, blended at full weight over its width. "the road SHALL
            // blend into the terrain material and suppress foliage placement along its width" —
            // the suppression half is the declared decoration, asked through
            // `suppresses_foliage()`.
            MaterialTexel& texel = tile.texel(i, j);
            texel = MaterialTexel{};
            texel.layer[0] = modifier.layer;
            texel.weight[0] = 255;
        }
    }
}

Expected<TerrainTile, Error> ModifierStack::evaluate(Allocator& allocator,
                                                     const TileCoord& coord) const noexcept {
    const u32 pad = halo_samples(coord);
    Expected<Padded, Error> generated = generate(allocator, coord, pad);
    if (!generated) {
        return make_unexpected(generated.error());
    }
    Padded grid = std::move(generated.value());

    for (u32 index = 0; index < modifiers_.size(); ++index) {
        if (!modifiers_[index].enabled) {
            continue;
        }
        apply(modifiers_[index], index, coord, grid);
    }

    Expected<TerrainTile, Error> made =
        TerrainTile::create(allocator, layout_, coord, layout_.height_min);
    if (!made) {
        return made;
    }
    TerrainTile tile = std::move(made.value());
    for (u32 j = 0; j < kTileVerts; ++j) {
        for (u32 i = 0; i < kTileVerts; ++i) {
            tile.set_stored(i, j, quantise_height(layout_, grid.at(i + pad, j + pad)));
        }
    }
    for (const Modifier& modifier : modifiers_.span()) {
        if (modifier.enabled) {
            write_material(modifier, coord, tile);
        }
    }
    tile.refresh_extent(layout_);
    tile.derivation_key = derivation_key(coord);
    return tile;
}

u64 ModifierStack::derivation_key(const TileCoord& coord) const noexcept {
    u64 key = hash_integer(layout_.signature(), kTerrainHashSeed);
    key = hash_combine(key, tile_id_of(layout_, coord).value);
    key = hash_combine(key, seed_);
    key = hash_combine(key, static_cast<u64>(std::bit_cast<u32>(generator_.period)));
    key = hash_combine(key, static_cast<u64>(std::bit_cast<u32>(generator_.amplitude)));
    key = hash_combine(key, generator_.octaves);
    key = hash_combine(key, static_cast<u64>(std::bit_cast<u32>(generator_.base_height)));
    // In stack ORDER, and only the enabled ones: reordering changes the key, and so does disabling
    // one, because both change what the tile is.
    for (const Modifier& modifier : modifiers_.span()) {
        if (!modifier.enabled) {
            continue;
        }
        key = hash_combine(key, static_cast<u64>(modifier.kind));
        key = hash_combine(key, static_cast<u64>(std::bit_cast<u32>(modifier.amplitude)));
        key = hash_combine(key, static_cast<u64>(std::bit_cast<u32>(modifier.period)));
        key = hash_combine(key, static_cast<u64>(std::bit_cast<u32>(modifier.height)));
        key = hash_combine(key, static_cast<u64>(std::bit_cast<u32>(modifier.radius)));
        key = hash_combine(key, modifier.iterations);
        key = hash_combine(key, modifier.layer);
        key = hash_combine(key, static_cast<u64>(std::bit_cast<u64>(modifier.bounds.min_x)));
        key = hash_combine(key, static_cast<u64>(std::bit_cast<u64>(modifier.bounds.min_z)));
        key = hash_combine(key, static_cast<u64>(std::bit_cast<u64>(modifier.bounds.max_x)));
        key = hash_combine(key, static_cast<u64>(std::bit_cast<u64>(modifier.bounds.max_z)));
        for (usize point = 0; point < modifier.point_count; ++point) {
            const TerrainPoint& at = points_[modifier.first_point + point];
            key = hash_combine(key, static_cast<u64>(std::bit_cast<u64>(at.x)));
            key = hash_combine(key, static_cast<u64>(std::bit_cast<u64>(at.z)));
        }
        for (usize sample = 0;
             sample < static_cast<usize>(modifier.stamp_edge) * modifier.stamp_edge; ++sample) {
            key = hash_combine(
                key,
                static_cast<u64>(std::bit_cast<u32>(samples_[modifier.first_sample + sample])));
        }
    }
    return key;
}

Status ModifierStack::dirty_tiles(u32 index, u8 level, Array<TileCoord>& out) const noexcept {
    if (index >= modifiers_.size()) {
        return fail(ErrorCode::OutOfRange, "terrain: no such modifier");
    }
    const TerrainBounds reach = modifier_reach(modifiers_[index]);
    const TileCoord low = tile_at(layout_, reach.min_x, reach.min_z, level);
    const TileCoord high = tile_at(layout_, reach.max_x, reach.max_z, level);
    for (i32 z = low.z; z <= high.z; ++z) {
        for (i32 x = low.x; x <= high.x; ++x) {
            if (Status pushed = out.push_back(TileCoord{x, z, level}); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

Status ModifierStack::flatten(TerrainStore& out, i32 min_tile_x, i32 min_tile_z, i32 max_tile_x,
                              i32 max_tile_z, u8 level) const noexcept {
    for (i32 z = min_tile_z; z <= max_tile_z; ++z) {
        for (i32 x = min_tile_x; x <= max_tile_x; ++x) {
            Expected<TerrainTile, Error> tile = evaluate(out.allocator(), TileCoord{x, z, level});
            if (!tile) {
                return Status{make_unexpected(tile.error())};
            }
            if (Status inserted = out.insert(std::move(tile.value())); !inserted) {
                return inserted;
            }
        }
    }
    return ok();
}

}  // namespace cy::terrain
