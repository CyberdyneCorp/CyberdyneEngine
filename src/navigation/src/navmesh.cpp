// The navigation mesh. See cy/navigation/navmesh.h for the argument; this file is the tiling, the
// adjacency and the point queries every other file in this module is written against.

#include <cy/core/base/assert.h>
#include <cy/navigation/navmesh.h>

#include <cmath>

namespace cy::navigation {
namespace {

constexpr f32 kEdgeEpsilon = 0.01f;  ///< a centimetre: two tiles' border vertices are cooked apart

[[nodiscard]] bool same_position(Vec3 a, Vec3 b) noexcept {
    return length_squared(a - b) <= (kEdgeEpsilon * kEdgeEpsilon);
}

/// The closest point to `p` on the triangle `a b c`. The standard Voronoi-region form: it never
/// divides by a degenerate area, which matters because a cooked mesh does contain slivers.
[[nodiscard]] Vec3 closest_on_triangle(Vec3 a, Vec3 b, Vec3 c, Vec3 p) noexcept {
    const Vec3 ab = b - a;
    const Vec3 ac = c - a;
    const Vec3 ap = p - a;
    const f32 d1 = dot(ab, ap);
    const f32 d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        return a;
    }
    const Vec3 bp = p - b;
    const f32 d3 = dot(ab, bp);
    const f32 d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        return b;
    }
    const f32 vc = (d1 * d4) - (d3 * d2);
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const f32 denominator = d1 - d3;
        const f32 v = (denominator != 0.0f) ? (d1 / denominator) : 0.0f;
        return a + (ab * v);
    }
    const Vec3 cp = p - c;
    const f32 d5 = dot(ab, cp);
    const f32 d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        return c;
    }
    const f32 vb = (d5 * d2) - (d1 * d6);
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const f32 denominator = d2 - d6;
        const f32 w = (denominator != 0.0f) ? (d2 / denominator) : 0.0f;
        return a + (ac * w);
    }
    const f32 va = (d3 * d6) - (d5 * d4);
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const f32 denominator = (d4 - d3) + (d5 - d6);
        const f32 w = (denominator != 0.0f) ? ((d4 - d3) / denominator) : 0.0f;
        return b + ((c - b) * w);
    }
    const f32 denominator = va + vb + vc;
    if (denominator == 0.0f) {
        return a;
    }
    const f32 v = vb / denominator;
    const f32 w = vc / denominator;
    return a + (ab * v) + (ac * w);
}

/// Is `p` inside the convex polygon, seen from above? Winding-agnostic: it accepts a point that is
/// on the same side of every edge, whichever side that is.
[[nodiscard]] bool inside_xz(const Vec3* verts, u32 count, Vec3 p) noexcept {
    bool positive = false;
    bool negative = false;
    for (u32 index = 0; index < count; ++index) {
        const Vec3 a = verts[index];
        const Vec3 b = verts[(index + 1) % count];
        const f32 side = ((b.x - a.x) * (p.z - a.z)) - ((b.z - a.z) * (p.x - a.x));
        if (side > 1e-5f) {
            positive = true;
        } else if (side < -1e-5f) {
            negative = true;
        }
        if (positive && negative) {
            return false;
        }
    }
    return true;
}

}  // namespace

NavAreaCosts NavAreaCosts::uniform() noexcept {
    NavAreaCosts costs;
    for (f32& value : costs.multiplier) {
        value = 1.0f;
    }
    return costs;
}

bool NavObstacleShape::contains(Vec3 point) const noexcept {
    if (radius > 0.0f) {
        const f32 dx = point.x - centre.x;
        const f32 dz = point.z - centre.z;
        if (((dx * dx) + (dz * dz)) > (radius * radius)) {
            return false;
        }
        if (height <= 0.0f) {
            return true;
        }
        return point.y >= (centre.y - 0.01f) && point.y <= (centre.y + height);
    }
    return point.x >= bounds.min.x && point.x <= bounds.max.x && point.y >= bounds.min.y &&
           point.y <= bounds.max.y && point.z >= bounds.min.z && point.z <= bounds.max.z;
}

Aabb NavObstacleShape::extent() const noexcept {
    if (radius > 0.0f) {
        return Aabb::from_min_max(Vec3{centre.x - radius, centre.y, centre.z - radius},
                                  Vec3{centre.x + radius, centre.y + height, centre.z + radius});
    }
    return bounds;
}

void NavTileData::finalise() noexcept {
    bounds = Aabb::empty();
    for (const Vec3& vertex : vertices_.span()) {
        bounds.grow(vertex);
    }
    for (NavPoly& poly : polys_.span()) {
        Vec3 sum;
        u32 counted = 0;
        for (u32 corner = 0; corner < poly.corner_count; ++corner) {
            const u32 index = corners_[poly.first_corner + corner];
            if (index < vertices_.size()) {
                sum = sum + vertices_[index];
                ++counted;
            }
        }
        poly.centre = (counted != 0) ? (sum * (1.0f / static_cast<f32>(counted))) : Vec3{};
    }
}

// --- Construction
// ---------------------------------------------------------------------------------

NavMesh::NavMesh(Allocator& allocator, Name name, f32 tile_size) noexcept
    : name_(name),
      tile_size_((tile_size > 0.0f) ? tile_size : 32.0f),
      tiles_(allocator),
      tile_coords_(allocator),
      obstacles_(allocator),
      links_(allocator) {}

// --- Tiles
// ------------------------------------------------------------------------------------------

u32 NavMesh::tile_slot(TileCoord coord) const noexcept {
    for (usize index = 0; index < tile_coords_.size(); ++index) {
        if (tiles_[index].resident && tile_coords_[index] == coord) {
            return static_cast<u32>(index);
        }
    }
    return 0xFFFFFFFFu;
}

bool NavMesh::tile_resident(TileCoord coord) const noexcept {
    return tile_slot(coord) != 0xFFFFFFFFu;
}

TileCoord NavMesh::tile_coord(u32 slot) const noexcept {
    return (slot < tile_coords_.size()) ? tile_coords_[slot] : TileCoord{};
}

Aabb NavMesh::tile_bounds(u32 slot) const noexcept {
    const Tile* tile = resident_tile(slot);
    return (tile != nullptr) ? tile->bounds : Aabb::empty();
}

TileCoord NavMesh::coord_of(Vec3 position) const noexcept {
    return TileCoord{static_cast<i32>(std::floor(position.x / tile_size_)),
                     static_cast<i32>(std::floor(position.z / tile_size_)), 0};
}

const NavMesh::Tile* NavMesh::resident_tile(u32 slot) const noexcept {
    if (slot >= tiles_.size() || !tiles_[slot].resident) {
        return nullptr;
    }
    return &tiles_[slot];
}

Expected<u32, Error> NavMesh::acquire_slot(TileCoord coord) noexcept {
    const u32 existing = tile_slot(coord);
    if (existing != 0xFFFFFFFFu) {
        return existing;
    }
    for (usize index = 0; index < tiles_.size(); ++index) {
        if (!tiles_[index].resident) {
            tile_coords_[index] = coord;
            return static_cast<u32>(index);
        }
    }
    if (tiles_.size() >= 0xFFFFFu) {
        return fail(ErrorCode::OutOfRange, "a navigation mesh holds at most 2^20 tiles");
    }
    if (Expected<Tile*, Error> slot = tiles_.emplace_back(allocator()); !slot) {
        return make_unexpected(slot.error());
    }
    if (Status pushed = tile_coords_.push_back(coord); !pushed) {
        tiles_.pop_back();
        return make_unexpected(pushed.error());
    }
    return static_cast<u32>(tiles_.size() - 1);
}

void NavMesh::disconnect_tile(u32 slot) noexcept {
    // Every neighbour entry that names this slot, cleared. Walking every resident tile is O(total
    // polygons) and happens once per removal; the alternative is a reverse index that has to be
    // kept correct through every rebuild, and a wrong reverse index is a dangling reference.
    for (Tile& tile : tiles_.span()) {
        if (!tile.resident) {
            continue;
        }
        for (PolyRef& neighbour : tile.neighbours.span()) {
            if (neighbour.valid() && neighbour.tile() == slot) {
                neighbour = kInvalidPoly;
            }
        }
    }
}

/// Adjacency WITHIN one tile. Two polygons share an edge when they name the same pair of vertex
/// indices in OPPOSITE ORDER — the cooked mesh welds its vertices, so this is an integer comparison
/// and not a geometric one, and the winding is what distinguishes a shared edge from two edges that
/// happen to run the same way.
u32 NavMesh::connect_within(u32 slot) noexcept {
    Tile& tile = tiles_[slot];
    u32 matched = 0;
    for (usize poly_index = 0; poly_index < tile.polys.size(); ++poly_index) {
        const NavPoly poly = tile.polys[poly_index];
        for (u32 edge = 0; edge < poly.corner_count; ++edge) {
            const usize here = poly.first_corner + edge;
            if (tile.neighbours[here].valid()) {
                continue;
            }
            const u32 v0 = tile.corners[here];
            const u32 v1 = tile.corners[poly.first_corner + ((edge + 1) % poly.corner_count)];

            for (usize other_index = poly_index + 1; other_index < tile.polys.size();
                 ++other_index) {
                const NavPoly other = tile.polys[other_index];
                bool joined = false;
                for (u32 other_edge = 0; other_edge < other.corner_count && !joined; ++other_edge) {
                    const usize there = other.first_corner + other_edge;
                    if (tile.neighbours[there].valid()) {
                        continue;
                    }
                    const u32 w0 = tile.corners[there];
                    const u32 w1 =
                        tile.corners[other.first_corner + ((other_edge + 1) % other.corner_count)];
                    if (v0 != w1 || v1 != w0) {
                        continue;
                    }
                    tile.neighbours[here] =
                        PolyRef::make(slot, tile.salt, static_cast<u32>(other_index));
                    tile.neighbours[there] =
                        PolyRef::make(slot, tile.salt, static_cast<u32>(poly_index));
                    ++matched;
                    joined = true;
                }
                if (joined) {
                    break;
                }
            }
        }
    }
    return matched;
}

/// One unmatched border edge of `slot`, against every unmatched border edge of `other_slot`.
///
/// Two tiles cooked separately do not share vertex INDICES, so the match is geometric and over
/// border edges only. `navigation` requires a rebuilt tile to have "neighbouring tiles' adjacency
/// updated", and this is that update seen from the tile being published.
bool NavMesh::connect_across(u32 slot, u32 other_slot, usize poly_index, usize here, Vec3 a0,
                             Vec3 a1) noexcept {
    for (usize other_index = 0; other_index < tiles_[other_slot].polys.size(); ++other_index) {
        const NavPoly other = tiles_[other_slot].polys[other_index];
        for (u32 other_edge = 0; other_edge < other.corner_count; ++other_edge) {
            const usize there = other.first_corner + other_edge;
            if (tiles_[other_slot].neighbours[there].valid()) {
                continue;
            }
            const Vec3 b0 = tiles_[other_slot].vertices[tiles_[other_slot].corners[there]];
            const Vec3 b1 =
                tiles_[other_slot]
                    .vertices[tiles_[other_slot].corners[other.first_corner +
                                                         ((other_edge + 1) % other.corner_count)]];
            if (!same_position(a0, b1) || !same_position(a1, b0)) {
                continue;
            }
            // BOTH SIDES, here. An adjacency written one way is not an adjacency: a search from the
            // far tile would not find its way back, and the asymmetry would only show up in a path
            // that happened to start on the other side of the seam.
            tiles_[slot].neighbours[here] =
                PolyRef::make(other_slot, tiles_[other_slot].salt, static_cast<u32>(other_index));
            tiles_[other_slot].neighbours[there] =
                PolyRef::make(slot, tiles_[slot].salt, static_cast<u32>(poly_index));
            return true;
        }
    }
    return false;
}

Status NavMesh::connect_tile(u32 slot, u32& internal_edges, u32& border_edges) noexcept {
    internal_edges = connect_within(slot);
    border_edges = 0;

    // Only the four tiles that touch this one at its own layer: a tile above or below is a separate
    // surface and is joined by an off-mesh link, never by a shared edge.
    const TileCoord coord = tile_coords_[slot];
    const TileCoord adjacent[4] = {
        TileCoord{coord.x - 1, coord.z, coord.layer}, TileCoord{coord.x + 1, coord.z, coord.layer},
        TileCoord{coord.x, coord.z - 1, coord.layer}, TileCoord{coord.x, coord.z + 1, coord.layer}};
    for (const TileCoord& neighbour_coord : adjacent) {
        const u32 other_slot = tile_slot(neighbour_coord);
        if (other_slot == 0xFFFFFFFFu) {
            continue;
        }
        for (usize poly_index = 0; poly_index < tiles_[slot].polys.size(); ++poly_index) {
            const NavPoly poly = tiles_[slot].polys[poly_index];
            for (u32 edge = 0; edge < poly.corner_count; ++edge) {
                const usize here = poly.first_corner + edge;
                if (tiles_[slot].neighbours[here].valid()) {
                    continue;
                }
                const Vec3 a0 = tiles_[slot].vertices[tiles_[slot].corners[here]];
                const Vec3 a1 =
                    tiles_[slot].vertices[tiles_[slot].corners[poly.first_corner +
                                                               ((edge + 1) % poly.corner_count)]];
                if (!connect_across(slot, other_slot, poly_index, here, a0, a1)) {
                    continue;
                }
                ++border_edges;
            }
        }
    }
    return ok();
}

Expected<TileChange, Error> NavMesh::add_tile(NavTileData&& data) noexcept {
    data.finalise();

    const Expected<u32, Error> slot = acquire_slot(data.coord);
    if (!slot) {
        return make_unexpected(slot.error());
    }
    const u32 index = *slot;

    TileChange change;
    change.coord = data.coord;
    change.tile = index;
    change.replaced_existing = tiles_[index].resident;

    // A rebuild retires the previous occupant BEFORE anything of the new one is visible: its salt
    // advances, so every corridor that named it stops resolving, and every neighbour's edge into it
    // is cleared so nothing walks into freed storage.
    if (tiles_[index].resident) {
        disconnect_tile(index);
        tiles_[index].salt = (tiles_[index].salt + 1) & 0xFFFFFu;
        if (tiles_[index].salt == 0) {
            tiles_[index].salt = 1;
        }
        tiles_[index].resident = false;
        --live_tiles_;
        ++tile_removals_;
    }

    Tile& tile = tiles_[index];
    tile.coord = data.coord;
    tile.bounds = data.bounds;
    tile.vertices.clear();
    tile.polys.clear();
    tile.corners.clear();
    tile.neighbours.clear();
    tile.obstacle_area.clear();

    if (Status appended = tile.vertices.append(data.vertices()); !appended) {
        return make_unexpected(appended.error());
    }
    if (Status appended = tile.polys.append(data.polys()); !appended) {
        return make_unexpected(appended.error());
    }
    if (Status appended = tile.corners.append(data.corners()); !appended) {
        return make_unexpected(appended.error());
    }
    if (Status sized = tile.neighbours.resize(tile.corners.size()); !sized) {
        return make_unexpected(sized.error());
    }
    for (PolyRef& neighbour : tile.neighbours.span()) {
        neighbour = kInvalidPoly;
    }
    if (Status sized = tile.obstacle_area.resize(tile.polys.size()); !sized) {
        return make_unexpected(sized.error());
    }
    for (u8& area : tile.obstacle_area.span()) {
        area = static_cast<u8>(kAreaCount);
    }

    tile_coords_[index] = data.coord;
    tiles_[index].resident = true;
    ++live_tiles_;
    ++tile_builds_;

    u32 internal_edges = 0;
    u32 border_edges = 0;
    if (Status connected = connect_tile(index, internal_edges, border_edges); !connected) {
        return make_unexpected(connected.error());
    }
    if (Status linked = rebuild_links(index); !linked) {
        return make_unexpected(linked.error());
    }
    reapply_obstacles(index);

    change.salt = tiles_[index].salt;
    change.polys = static_cast<u32>(tiles_[index].polys.size());
    change.internal_edges = internal_edges;
    change.border_edges = border_edges;
    ++version_;
    return change;
}

Status NavMesh::remove_tile(TileCoord coord) noexcept {
    const u32 slot = tile_slot(coord);
    if (slot == 0xFFFFFFFFu) {
        return fail(ErrorCode::NotFound, "no tile at that coordinate");
    }
    disconnect_tile(slot);
    Tile& tile = tiles_[slot];
    tile.salt = (tile.salt + 1) & 0xFFFFFu;
    if (tile.salt == 0) {
        tile.salt = 1;
    }
    tile.resident = false;
    tile.vertices.clear();
    tile.polys.clear();
    tile.corners.clear();
    tile.neighbours.clear();
    tile.links.clear();
    tile.link_starts.clear();
    tile.obstacle_area.clear();
    --live_tiles_;
    ++tile_removals_;
    ++version_;
    return ok();
}

// --- Polygons
// --------------------------------------------------------------------------------------

const NavPoly* NavMesh::poly(PolyRef ref) const noexcept {
    if (!ref.valid()) {
        return nullptr;
    }
    const Tile* tile = resident_tile(ref.tile());
    if (tile == nullptr || tile->salt != ref.salt() || ref.poly() >= tile->polys.size()) {
        return nullptr;
    }
    return &tile->polys[ref.poly()];
}

Span<const u32> NavMesh::poly_corners(PolyRef ref) const noexcept {
    const NavPoly* found = poly(ref);
    if (found == nullptr) {
        return {};
    }
    const Tile& tile = tiles_[ref.tile()];
    return {tile.corners.data() + found->first_corner, found->corner_count};
}

Span<const PolyRef> NavMesh::poly_neighbours(PolyRef ref) const noexcept {
    const NavPoly* found = poly(ref);
    if (found == nullptr) {
        return {};
    }
    const Tile& tile = tiles_[ref.tile()];
    return {tile.neighbours.data() + found->first_corner, found->corner_count};
}

Span<const Vec3> NavMesh::tile_vertices(u32 slot) const noexcept {
    const Tile* tile = resident_tile(slot);
    return (tile != nullptr) ? tile->vertices.span() : Span<const Vec3>{};
}

u32 NavMesh::tile_poly_count(u32 slot) const noexcept {
    const Tile* tile = resident_tile(slot);
    return (tile != nullptr) ? static_cast<u32>(tile->polys.size()) : 0;
}

PolyRef NavMesh::tile_poly(u32 slot, u32 index) const noexcept {
    const Tile* tile = resident_tile(slot);
    if (tile == nullptr || index >= tile->polys.size()) {
        return kInvalidPoly;
    }
    return PolyRef::make(slot, tile->salt, index);
}

u32 NavMesh::poly_vertices(PolyRef ref, Vec3* out, u32 capacity) const noexcept {
    const NavPoly* found = poly(ref);
    if (found == nullptr || out == nullptr) {
        return 0;
    }
    const Tile& tile = tiles_[ref.tile()];
    const u32 count = (found->corner_count < capacity) ? found->corner_count : capacity;
    for (u32 index = 0; index < count; ++index) {
        out[index] = tile.vertices[tile.corners[found->first_corner + index]];
    }
    return count;
}

bool NavMesh::portal(PolyRef from, PolyRef to, Vec3& left, Vec3& right) const noexcept {
    const NavPoly* source = poly(from);
    if (source == nullptr || !to.valid()) {
        return false;
    }
    const Tile& tile = tiles_[from.tile()];
    for (u32 edge = 0; edge < source->corner_count; ++edge) {
        const usize corner = source->first_corner + edge;
        if (tile.neighbours[corner] != to) {
            continue;
        }
        left = tile.vertices[tile.corners[corner]];
        right =
            tile.vertices[tile.corners[source->first_corner + ((edge + 1) % source->corner_count)]];
        return true;
    }
    return false;
}

bool NavMesh::contains_point(PolyRef ref, Vec3 point, f32 vertical_tolerance,
                             Vec3& on_poly) const noexcept {
    Vec3 verts[kMaxPolyVertices];
    const u32 count = poly_vertices(ref, verts, kMaxPolyVertices);
    if (count < 3) {
        return false;
    }
    if (!inside_xz(verts, count, point)) {
        return false;
    }
    Vec3 best = verts[0];
    f32 best_distance = math::kInfinity;
    for (u32 index = 1; index + 1 < count; ++index) {
        const Vec3 candidate = closest_on_triangle(verts[0], verts[index], verts[index + 1], point);
        const f32 distance = length_squared(candidate - point);
        if (distance < best_distance) {
            best_distance = distance;
            best = candidate;
        }
    }
    on_poly = Vec3{point.x, best.y, point.z};
    return std::fabs(best.y - point.y) <= vertical_tolerance;
}

PolyRef NavMesh::find_nearest(Vec3 point, Vec3 extents, AreaMask areas,
                              Vec3& nearest) const noexcept {
    const Aabb search = Aabb::from_center_extents(point, extents);
    PolyRef best = kInvalidPoly;
    f32 best_distance = math::kInfinity;
    nearest = point;

    for (usize slot = 0; slot < tiles_.size(); ++slot) {
        const Tile& tile = tiles_[slot];
        if (!tile.resident || !tile.bounds.intersects(search)) {
            continue;
        }
        for (usize poly_index = 0; poly_index < tile.polys.size(); ++poly_index) {
            const PolyRef ref =
                PolyRef::make(static_cast<u32>(slot), tile.salt, static_cast<u32>(poly_index));
            if ((area_bit(effective_area(ref)) & areas) == 0) {
                continue;
            }
            Vec3 verts[kMaxPolyVertices];
            const u32 count = poly_vertices(ref, verts, kMaxPolyVertices);
            if (count < 3) {
                continue;
            }
            for (u32 index = 1; index + 1 < count; ++index) {
                const Vec3 candidate =
                    closest_on_triangle(verts[0], verts[index], verts[index + 1], point);
                // THE CANDIDATE ITSELF MUST BE WITHIN `extents`, not merely in a tile that overlaps
                // them. Rejecting only whole tiles made this function answer "the nearest polygon
                // in the tile" however far away it was, which is not what its declaration says and
                // is not what its callers can survive: `add_link` would snap a jump's endpoint to
                // the far side of a hole rather than refusing it, `find_path` would silently start
                // a search from somewhere the agent is not, and a query over a hole would report
                // ground. `tests/test_build.cpp`'s erosion case is the regression.
                if (!search.contains(candidate)) {
                    continue;
                }
                const f32 distance = length_squared(candidate - point);
                if (distance < best_distance) {
                    best_distance = distance;
                    best = ref;
                    nearest = candidate;
                }
            }
        }
    }
    return best;
}

// --- Obstacles
// ---------------------------------------------------------------------------------------

void NavMesh::reapply_obstacles(u32 slot) noexcept {
    Tile* tile = (slot < tiles_.size() && tiles_[slot].resident) ? &tiles_[slot] : nullptr;
    if (tile == nullptr) {
        return;
    }
    for (u8& area : tile->obstacle_area.span()) {
        area = static_cast<u8>(kAreaCount);
    }
    for (const ObstacleEntry& entry : obstacles_.span()) {
        if (!entry.live || !tile->bounds.intersects(entry.shape.extent())) {
            continue;
        }
        for (usize poly_index = 0; poly_index < tile->polys.size(); ++poly_index) {
            if (entry.shape.contains(tile->polys[poly_index].centre)) {
                tile->obstacle_area[poly_index] = entry.shape.area;
            }
        }
    }
}

Expected<ObstacleId, Error> NavMesh::add_obstacle(const NavObstacleShape& shape) noexcept {
    ObstacleId id = kInvalidObstacle;
    for (usize index = 0; index < obstacles_.size(); ++index) {
        if (!obstacles_[index].live) {
            id = static_cast<ObstacleId>(index);
            break;
        }
    }
    if (id == kInvalidObstacle) {
        ObstacleEntry entry;
        entry.id = static_cast<ObstacleId>(obstacles_.size());
        if (Status pushed = obstacles_.push_back(entry); !pushed) {
            return make_unexpected(pushed.error());
        }
        id = entry.id;
    }
    obstacles_[id].shape = shape;
    obstacles_[id].id = id;
    obstacles_[id].live = true;
    for (usize slot = 0; slot < tiles_.size(); ++slot) {
        reapply_obstacles(static_cast<u32>(slot));
    }
    ++version_;
    return id;
}

Status NavMesh::remove_obstacle(ObstacleId id) noexcept {
    if (id >= obstacles_.size() || !obstacles_[id].live) {
        return fail(ErrorCode::NotFound, "no such navigation obstacle");
    }
    obstacles_[id].live = false;
    for (usize slot = 0; slot < tiles_.size(); ++slot) {
        reapply_obstacles(static_cast<u32>(slot));
    }
    ++version_;
    return ok();
}

u32 NavMesh::obstacle_count() const noexcept {
    u32 count = 0;
    for (const ObstacleEntry& entry : obstacles_.span()) {
        count += entry.live ? 1u : 0u;
    }
    return count;
}

AreaType NavMesh::effective_area(PolyRef ref) const noexcept {
    const NavPoly* found = poly(ref);
    if (found == nullptr) {
        return kAreaNull;
    }
    const u8 imposed = tiles_[ref.tile()].obstacle_area[ref.poly()];
    return (imposed < kAreaCount) ? static_cast<AreaType>(imposed) : found->area;
}

// --- Links
// ---------------------------------------------------------------------------------------------

Status NavMesh::rebuild_links(u32 slot) noexcept {
    Tile& tile = tiles_[slot];
    tile.links.clear();
    tile.link_starts.clear();

    // Re-snap first: a rebuilt tile has new polygon references, and a link that ended on the old
    // ones would be dropped silently. This is what makes a door that opens twice still a door.
    for (LinkEntry& entry : links_.span()) {
        if (!entry.live) {
            continue;
        }
        if (poly(entry.link.from_poly) == nullptr) {
            Vec3 nearest;
            entry.link.from_poly = find_nearest(entry.link.from, entry.snap, kAllAreas, nearest);
        }
        if (poly(entry.link.to_poly) == nullptr) {
            Vec3 nearest;
            entry.link.to_poly = find_nearest(entry.link.to, entry.snap, kAllAreas, nearest);
        }
    }

    if (Status sized = tile.link_starts.resize(tile.polys.size() + 1); !sized) {
        return sized;
    }
    for (u32& start : tile.link_starts.span()) {
        start = 0;
    }
    // Counting sort by polygon, so `links_from` is a span and not a filtered walk.
    for (const LinkEntry& entry : links_.span()) {
        if (!entry.live) {
            continue;
        }
        if (entry.link.from_poly.valid() && entry.link.from_poly.tile() == slot &&
            entry.link.from_poly.poly() < tile.polys.size()) {
            ++tile.link_starts[entry.link.from_poly.poly() + 1];
        }
        if (entry.link.bidirectional && entry.link.to_poly.valid() &&
            entry.link.to_poly.tile() == slot && entry.link.to_poly.poly() < tile.polys.size()) {
            ++tile.link_starts[entry.link.to_poly.poly() + 1];
        }
    }
    for (usize index = 1; index < tile.link_starts.size(); ++index) {
        tile.link_starts[index] += tile.link_starts[index - 1];
    }
    if (Status sized = tile.links.resize(tile.link_starts[tile.link_starts.size() - 1]); !sized) {
        return sized;
    }
    Array<u32> cursor(allocator());
    if (Status sized = cursor.resize(tile.polys.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < cursor.size(); ++index) {
        cursor[index] = tile.link_starts[index];
    }
    for (usize index = 0; index < links_.size(); ++index) {
        const LinkEntry& entry = links_[index];
        if (!entry.live) {
            continue;
        }
        if (entry.link.from_poly.valid() && entry.link.from_poly.tile() == slot &&
            entry.link.from_poly.poly() < tile.polys.size()) {
            tile.links[cursor[entry.link.from_poly.poly()]++] = static_cast<LinkId>(index);
        }
        if (entry.link.bidirectional && entry.link.to_poly.valid() &&
            entry.link.to_poly.tile() == slot && entry.link.to_poly.poly() < tile.polys.size()) {
            tile.links[cursor[entry.link.to_poly.poly()]++] = static_cast<LinkId>(index);
        }
    }
    return ok();
}

Expected<LinkId, Error> NavMesh::add_link(const NavLink& link, Vec3 snap) noexcept {
    LinkEntry entry;
    entry.link = link;
    entry.snap = snap;
    entry.live = true;

    Vec3 nearest;
    entry.link.from_poly = find_nearest(link.from, snap, kAllAreas, nearest);
    entry.link.to_poly = find_nearest(link.to, snap, kAllAreas, nearest);
    if (!entry.link.from_poly.valid() || !entry.link.to_poly.valid()) {
        return fail(ErrorCode::InvalidArgument,
                    "an off-mesh link's endpoints must both land on the navigation mesh");
    }

    LinkId id = kInvalidLink;
    for (usize index = 0; index < links_.size(); ++index) {
        if (!links_[index].live) {
            id = static_cast<LinkId>(index);
            break;
        }
    }
    if (id == kInvalidLink) {
        if (Status pushed = links_.push_back(entry); !pushed) {
            return make_unexpected(pushed.error());
        }
        id = static_cast<LinkId>(links_.size() - 1);
    } else {
        links_[id] = entry;
    }
    for (usize slot = 0; slot < tiles_.size(); ++slot) {
        if (tiles_[slot].resident) {
            if (Status rebuilt = rebuild_links(static_cast<u32>(slot)); !rebuilt) {
                return make_unexpected(rebuilt.error());
            }
        }
    }
    ++version_;
    return id;
}

Status NavMesh::remove_link(LinkId id) noexcept {
    if (id >= links_.size() || !links_[id].live) {
        return fail(ErrorCode::NotFound, "no such off-mesh link");
    }
    links_[id].live = false;
    for (usize slot = 0; slot < tiles_.size(); ++slot) {
        if (tiles_[slot].resident) {
            if (Status rebuilt = rebuild_links(static_cast<u32>(slot)); !rebuilt) {
                return rebuilt;
            }
        }
    }
    ++version_;
    return ok();
}

const NavLink* NavMesh::link(LinkId id) const noexcept {
    if (id >= links_.size() || !links_[id].live) {
        return nullptr;
    }
    return &links_[id].link;
}

Span<const LinkId> NavMesh::links_from(PolyRef ref) const noexcept {
    const NavPoly* found = poly(ref);
    if (found == nullptr) {
        return {};
    }
    const Tile& tile = tiles_[ref.tile()];
    if (tile.link_starts.size() <= ref.poly() + 1) {
        return {};
    }
    const u32 first = tile.link_starts[ref.poly()];
    const u32 last = tile.link_starts[ref.poly() + 1];
    return {tile.links.data() + first, last - first};
}

NavMesh::Stats NavMesh::stats() const noexcept {
    Stats result;
    result.tiles = live_tiles_;
    result.tile_builds = tile_builds_;
    result.tile_removals = tile_removals_;
    result.obstacles = obstacle_count();
    for (const Tile& tile : tiles_.span()) {
        if (!tile.resident) {
            continue;
        }
        result.polys += static_cast<u32>(tile.polys.size());
        result.vertices += static_cast<u32>(tile.vertices.size());
        for (const PolyRef& neighbour : tile.neighbours.span()) {
            if (!neighbour.valid()) {
                ++result.border_edges;
            }
        }
    }
    for (const LinkEntry& entry : links_.span()) {
        result.links += entry.live ? 1u : 0u;
    }
    return result;
}

}  // namespace cy::navigation
