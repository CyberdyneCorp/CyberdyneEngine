// Collision built from the gameplay surface, and the navigation surface built from collision.
// M10 task 2.2.

#include <cy/terrain/collision.h>

#include <cmath>

namespace cy::terrain {
namespace {

[[nodiscard]] u64 collision_key(const TileCoord& coord) noexcept {
    u64 value = hash_integer(static_cast<u64>(static_cast<u32>(coord.x)), kTerrainHashSeed);
    value = hash_combine(value, static_cast<u64>(static_cast<u32>(coord.z)));
    return hash_combine(value, coord.level);
}

}  // namespace

Expected<CollisionTile, Error> build_collision(Allocator& allocator, const TerrainStore& store,
                                               const HeightfieldSource& heights,
                                               const TileCoord& coord,
                                               const CollisionConfig& config) noexcept {
    const TerrainTile* tile = store.find(coord);
    if (tile == nullptr) {
        return fail(ErrorCode::NotFound, "terrain: that tile is not resident");
    }
    if (config.decimation == 0 || kTileQuads % config.decimation != 0) {
        return fail(ErrorCode::InvalidArgument,
                    "terrain: the collision decimation must divide the tile's quad count");
    }

    CollisionTile collision(allocator);
    collision.coord = coord;
    collision.bounds = tile_bounds(store.layout(), coord);
    collision.edge = (kTileQuads / config.decimation) + 1;
    collision.spacing =
        store.layout().sample_metres(coord.level) * static_cast<f32>(config.decimation);
    if (Status sized =
            collision.samples.resize(static_cast<usize>(collision.edge) * collision.edge);
        !sized) {
        return make_unexpected(sized.error());
    }

    f32 low = 0.0F;
    f32 high = 0.0F;
    bool first = true;
    for (u32 j = 0; j < collision.edge; ++j) {
        for (u32 i = 0; i < collision.edge; ++i) {
            const u32 fine_i = i * config.decimation;
            const u32 fine_j = j * config.decimation;
            const u32 quad_i = (fine_i < kTileQuads) ? fine_i : kTileQuads - 1;
            const u32 quad_j = (fine_j < kTileQuads) ? fine_j : kTileQuads - 1;
            if (tile->hole(quad_i, quad_j)) {
                // The sentinel `physics` already defines, so the mapping into a body is an
                // identity rather than a second bitmask to keep in step.
                collision.samples[(j * collision.edge) + i] = physics::kHeightFieldHole;
                ++collision.holes;
                continue;
            }
            const f32 height = heights.sample_height(coord, fine_i, fine_j);
            collision.samples[(j * collision.edge) + i] = height;
            low = (first || height < low) ? height : low;
            high = (first || height > high) ? height : high;
            first = false;
        }
    }
    collision.min_height = low;
    collision.max_height = high;
    return collision;
}

physics::HeightFieldDescription describe_collision(const CollisionTile& tile) noexcept {
    physics::HeightFieldDescription description;
    description.samples = tile.samples.data();
    description.sample_count_x = tile.edge;
    description.sample_count_z = tile.edge;
    // The shape's local origin is the tile's minimum corner. A body placed at the simulation origin
    // `world::simulation_origin()` gives for the region then puts the samples where the query path
    // says they are.
    description.offset = Vec3{0.0F, 0.0F, 0.0F};
    description.scale = Vec3{tile.spacing, 1.0F, tile.spacing};
    return description;
}

TerrainCollision::TerrainCollision(Allocator& allocator, const TerrainStore& store,
                                   const CollisionConfig& config) noexcept
    : allocator_(&allocator),
      store_(&store),
      config_(config),
      tiles_(allocator),
      index_(allocator) {}

const CollisionTile* TerrainCollision::find(const TileCoord& coord) const noexcept {
    const usize* slot = index_.find(collision_key(coord));
    if (slot == nullptr || !(tiles_[*slot].coord == coord)) {
        return nullptr;
    }
    return &tiles_[*slot];
}

Status TerrainCollision::build_into(const HeightfieldSource& heights,
                                    const TileCoord& coord) noexcept {
    Expected<CollisionTile, Error> built =
        build_collision(*allocator_, *store_, heights, coord, config_);
    if (!built) {
        return Status{make_unexpected(built.error())};
    }
    const usize* slot = index_.find(collision_key(coord));
    if (slot != nullptr && tiles_[*slot].coord == coord) {
        tiles_[*slot] = std::move(built.value());
        return ok();
    }
    if (Status pushed = tiles_.push_back(std::move(built.value())); !pushed) {
        return pushed;
    }
    Expected<usize*, Error> mapped = index_.insert(collision_key(coord), tiles_.size() - 1);
    if (!mapped) {
        tiles_.remove_unordered(tiles_.size() - 1);
        return Status{make_unexpected(mapped.error())};
    }
    return ok();
}

Expected<CollisionRegistration, Error> TerrainCollision::activate(
    const HeightfieldSource& heights, Span<const TileCoord> tiles) noexcept {
    CollisionRegistration registration;
    for (const TileCoord& coord : tiles) {
        if (Status built = build_into(heights, coord); !built) {
            return make_unexpected(built.error());
        }
        const CollisionTile* collision = find(coord);
        ++registration.tiles;
        registration.samples += collision->samples.size();
        registration.holes += collision->holes;
    }
    return registration;
}

u32 TerrainCollision::deactivate(Span<const TileCoord> tiles) noexcept {
    u32 dropped = 0;
    for (const TileCoord& coord : tiles) {
        const usize* slot = index_.find(collision_key(coord));
        if (slot == nullptr || !(tiles_[*slot].coord == coord)) {
            continue;
        }
        const usize victim = *slot;
        (void)index_.remove(collision_key(coord));
        const usize last = tiles_.size() - 1;
        if (victim != last) {
            const TileCoord moved = tiles_[last].coord;
            tiles_[victim] = std::move(tiles_[last]);
            if (usize* moved_slot = index_.find(collision_key(moved)); moved_slot != nullptr) {
                *moved_slot = victim;
            }
        }
        tiles_.remove_unordered(last);
        ++dropped;
    }
    return dropped;
}

Expected<u32, Error> TerrainCollision::invalidate(const HeightfieldSource& heights,
                                                  const TerrainBounds& bounds) noexcept {
    // The tiles are collected first: `build_into` may move entries within `tiles_`, and rebuilding
    // while iterating it would rebuild one tile twice and skip another.
    Array<TileCoord> affected(*allocator_);
    for (const CollisionTile& tile : tiles_.span()) {
        if (!tile.bounds.overlaps(bounds)) {
            continue;
        }
        if (Status pushed = affected.push_back(tile.coord); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    for (const TileCoord& coord : affected.span()) {
        if (Status built = build_into(heights, coord); !built) {
            return make_unexpected(built.error());
        }
    }
    return static_cast<u32>(affected.size());
}

// --- The navigation contribution
// ------------------------------------------------------------

navigation::NavSourceGeometry NavContribution::source() const noexcept {
    navigation::NavSourceGeometry geometry;
    geometry.vertices = vertices.span();
    geometry.indices = indices.span();
    geometry.area = area.span();
    return geometry;
}

TerrainNavigation::TerrainNavigation(Allocator& allocator) noexcept
    : layers_(allocator), fields_(allocator), dirty_(allocator) {}

Status TerrainNavigation::add_layer_mapping(const NavLayerMapping& mapping) noexcept {
    return layers_.push_back(mapping);
}

Status TerrainNavigation::add_field_mapping(const NavFieldMapping& mapping) noexcept {
    return fields_.push_back(mapping);
}

void TerrainNavigation::resolve(u8 layer, const environment::FieldStore* fields, f64 x, f32 height,
                                f64 z, navigation::AreaType& area, f32& cost) const noexcept {
    area = navigation::kAreaGround;
    cost = 1.0F;
    for (const NavLayerMapping& mapping : layers_.span()) {
        if (mapping.layer == layer) {
            area = mapping.area;
            cost = mapping.cost;
            break;
        }
    }
    if (fields == nullptr) {
        return;
    }
    for (const NavFieldMapping& mapping : fields_.span()) {
        if (!mapping.field.is_valid()) {
            continue;
        }
        const environment::FieldSample sampled = fields->sample_deterministic(
            mapping.field, world::WorldVec3d{x, static_cast<f64>(height), z});
        const f32 value = sampled.value.x();
        if (value < mapping.low || value > mapping.high) {
            continue;
        }
        // The runtime state wins over the cooked one. See the declaration's comment.
        area = mapping.area;
        cost = mapping.cost;
    }
}

/// The collision samples, in absolute world coordinates, as the navigation surface's vertices.
///
/// Deriving them from the RENDER mesh instead would give agents a surface that differs from the one
/// bodies rest on wherever the two resolutions differ, which is everywhere.
Status TerrainNavigation::emit_vertices(const CollisionTile& collision,
                                        NavContribution& out) noexcept {
    for (u32 j = 0; j < collision.edge; ++j) {
        for (u32 i = 0; i < collision.edge; ++i) {
            const f32 height = collision.samples[(j * collision.edge) + i];
            const f32 usable = (height == physics::kHeightFieldHole) ? 0.0F : height;
            const Vec3 vertex{static_cast<f32>(collision.bounds.min_x) +
                                  (static_cast<f32>(i) * collision.spacing),
                              usable,
                              static_cast<f32>(collision.bounds.min_z) +
                                  (static_cast<f32>(j) * collision.spacing)};
            if (Status pushed = out.vertices.push_back(vertex); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

/// True when any corner of the quad is a hole. An agent cannot path over a cave mouth.
[[nodiscard]] bool quad_is_hole(const CollisionTile& collision, u32 i, u32 j) noexcept {
    for (u32 corner = 0; corner < 4; ++corner) {
        const u32 ci = i + (corner & 1U);
        const u32 cj = j + (corner >> 1U);
        if (collision.samples[(cj * collision.edge) + ci] == physics::kHeightFieldHole) {
            return true;
        }
    }
    return false;
}

Status TerrainNavigation::emit_quad(const TerrainTile& tile, const CollisionTile& collision,
                                    const environment::FieldStore* fields, u32 i, u32 j,
                                    NavContribution& out) const noexcept {
    const u32 base = (j * collision.edge) + i;
    const u32 corners[6] = {base,     base + collision.edge, base + 1,
                            base + 1, base + collision.edge, base + collision.edge + 1};
    for (const u32 corner : corners) {
        if (Status pushed = out.indices.push_back(corner); !pushed) {
            return pushed;
        }
    }

    const u32 decimation = kTileQuads / (collision.edge - 1);
    const u32 texel_i = i * decimation;
    const u32 texel_j = j * decimation;
    const u8 layer = tile.texel((texel_i < kTileTexels) ? texel_i : kTileTexels - 1,
                                (texel_j < kTileTexels) ? texel_j : kTileTexels - 1)
                         .dominant();
    const f64 x = collision.bounds.min_x +
                  ((static_cast<f64>(i) + 0.5) * static_cast<f64>(collision.spacing));
    const f64 z = collision.bounds.min_z +
                  ((static_cast<f64>(j) + 0.5) * static_cast<f64>(collision.spacing));
    navigation::AreaType area = navigation::kAreaGround;
    f32 cost = 1.0F;
    resolve(layer, fields, x, collision.samples[base], z, area, cost);
    for (u32 triangle = 0; triangle < 2; ++triangle) {
        if (Status pushed = out.area.push_back(area); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Expected<NavContribution, Error> TerrainNavigation::contribute(
    Allocator& allocator, const TerrainStore& store, const CollisionTile& collision,
    const environment::FieldStore* fields) const noexcept {
    const TerrainTile* tile = store.find(collision.coord);
    if (tile == nullptr) {
        return fail(ErrorCode::NotFound, "terrain: that tile is not resident");
    }

    NavContribution contribution(allocator);
    contribution.coord = collision.coord;
    contribution.bounds = collision.bounds;
    if (Status emitted = emit_vertices(collision, contribution); !emitted) {
        return make_unexpected(emitted.error());
    }

    for (u32 j = 0; j + 1 < collision.edge; ++j) {
        for (u32 i = 0; i + 1 < collision.edge; ++i) {
            if (quad_is_hole(collision, i, j)) {
                ++contribution.holes;
                continue;
            }
            if (Status emitted = emit_quad(*tile, collision, fields, i, j, contribution);
                !emitted) {
                return make_unexpected(emitted.error());
            }
        }
    }
    return contribution;
}

Status TerrainNavigation::mark_dirty(Span<const TerrainBounds> regions) noexcept {
    for (const TerrainBounds& region : regions) {
        if (Status pushed = dirty_.push_back(region); !pushed) {
            return pushed;
        }
    }
    return ok();
}

}  // namespace cy::terrain
