// The region graph above the polygon graph. See cy/navigation/hierarchy.h for the argument.

#include <cy/core/base/assert.h>
#include <cy/navigation/hierarchy.h>

#include <cmath>

namespace cy::navigation {
namespace {

[[nodiscard]] f32 region_distance(const NavRegion& a, const NavRegion& b) noexcept {
    return length(b.centre - a.centre);
}

}  // namespace

NavHierarchy::NavHierarchy(Allocator& allocator) noexcept
    : regions_(allocator), tiles_(allocator), queue_(allocator) {}

u32 NavHierarchy::tile_entry(TileCoord coord) const noexcept {
    for (usize index = 0; index < tiles_.size(); ++index) {
        if (tiles_[index].live && tiles_[index].coord == coord) {
            return static_cast<u32>(index);
        }
    }
    return 0xFFFFFFFFu;
}

Expected<u32, Error> NavHierarchy::acquire_tile(TileCoord coord) noexcept {
    const u32 existing = tile_entry(coord);
    if (existing != 0xFFFFFFFFu) {
        return existing;
    }
    for (usize index = 0; index < tiles_.size(); ++index) {
        if (!tiles_[index].live) {
            tiles_[index].coord = coord;
            tiles_[index].live = true;
            tiles_[index].regions.clear();
            tiles_[index].region_of_poly.clear();
            return static_cast<u32>(index);
        }
    }
    TileEntry entry(tiles_.allocator());
    entry.coord = coord;
    entry.live = true;
    if (Status pushed = tiles_.push_back(std::move(entry)); !pushed) {
        return make_unexpected(pushed.error());
    }
    return static_cast<u32>(tiles_.size() - 1);
}

Expected<RegionId, Error> NavHierarchy::acquire_region() noexcept {
    for (usize index = 0; index < regions_.size(); ++index) {
        if (!regions_[index].live) {
            regions_[index].live = true;
            regions_[index].region = NavRegion{};
            regions_[index].edges.clear();
            ++live_regions_;
            return static_cast<RegionId>(index);
        }
    }
    RegionEntry entry(regions_.allocator());
    entry.live = true;
    if (Status pushed = regions_.push_back(std::move(entry)); !pushed) {
        return make_unexpected(pushed.error());
    }
    ++live_regions_;
    return static_cast<RegionId>(regions_.size() - 1);
}

void NavHierarchy::drop_regions_of(u32 entry, HierarchyUpdate& update) noexcept {
    TileEntry& tile = tiles_[entry];
    for (const RegionId id : tile.regions.span()) {
        if (id < regions_.size() && regions_[id].live) {
            update.edges_removed += static_cast<u32>(regions_[id].edges.size());
            regions_[id].live = false;
            regions_[id].edges.clear();
            --live_regions_;
            ++update.regions_removed;
        }
    }
    tile.regions.clear();
    tile.region_of_poly.clear();

    // Every edge that named one of those regions, dropped. A full scan rather than a reverse index:
    // the reverse index would have to stay correct through every rebuild, and a wrong one is a
    // dangling region id — the same argument `NavMesh::disconnect_tile` makes about neighbours.
    for (RegionEntry& region : regions_.span()) {
        if (!region.live) {
            continue;
        }
        usize keep = 0;
        for (usize index = 0; index < region.edges.size(); ++index) {
            const RegionId target = region.edges[index].to;
            if (target < regions_.size() && regions_[target].live) {
                region.edges[keep] = region.edges[index];
                ++keep;
            } else {
                ++update.edges_removed;
            }
        }
        while (region.edges.size() > keep) {
            region.edges.pop_back();
        }
    }
}

Status NavHierarchy::connect(RegionId from, RegionId to, f32 cost,
                             HierarchyUpdate& update) noexcept {
    if (from == to || from >= regions_.size() || to >= regions_.size()) {
        return ok();
    }
    for (const RegionEdge& edge : regions_[from].edges.span()) {
        if (edge.to == to) {
            return ok();
        }
    }
    if (Status pushed = regions_[from].edges.push_back(RegionEdge{to, cost}); !pushed) {
        return pushed;
    }
    if (Status pushed = regions_[to].edges.push_back(RegionEdge{from, cost}); !pushed) {
        return pushed;
    }
    update.edges_added += 2;
    return ok();
}

/// One connected component of one tile, by flood fill over adjacency INSIDE that tile.
///
/// Polygon order is the cooked tile's own order, so the component numbering is stable for a given
/// tile — which is what makes a region's identity survive a rebuild that did not change the mesh.
Expected<RegionId, Error> NavHierarchy::flood_region(const NavMesh& mesh, u32 slot, u32 entry,
                                                     u32 seed, u32 component,
                                                     HierarchyUpdate& update) noexcept {
    Expected<RegionId, Error> id = acquire_region();
    if (!id) {
        return id;
    }
    ++update.regions_added;
    if (Status pushed = tiles_[entry].regions.push_back(*id); !pushed) {
        return make_unexpected(pushed.error());
    }

    NavRegion& region = regions_[*id].region;
    region.coord = tiles_[entry].coord;
    region.component = component;
    region.resident = true;
    region.bounds = Aabb::empty();
    Vec3 sum{0.0F, 0.0F, 0.0F};

    queue_.clear();
    if (Status pushed = queue_.push_back(seed); !pushed) {
        return make_unexpected(pushed.error());
    }
    tiles_[entry].region_of_poly[seed] = *id;
    for (usize cursor = 0; cursor < queue_.size(); ++cursor) {
        const u32 poly_index = queue_[cursor];
        ++update.polys_visited;
        const PolyRef ref = mesh.tile_poly(slot, poly_index);
        const NavPoly* poly = mesh.poly(ref);
        if (poly == nullptr) {
            continue;
        }
        sum += poly->centre;
        ++region.polys;
        Vec3 corners[kMaxPolyVertices];
        const u32 corner_count = mesh.poly_vertices(ref, corners, kMaxPolyVertices);
        for (u32 corner = 0; corner < corner_count; ++corner) {
            region.bounds.grow(corners[corner]);
        }
        for (const PolyRef neighbour : mesh.poly_neighbours(ref)) {
            if (!neighbour.valid() || neighbour.tile() != slot ||
                tiles_[entry].region_of_poly[neighbour.poly()] != kInvalidRegion) {
                continue;
            }
            tiles_[entry].region_of_poly[neighbour.poly()] = *id;
            if (Status pushed = queue_.push_back(neighbour.poly()); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
    }
    region.centre = (region.polys > 0) ? (sum * (1.0F / static_cast<f32>(region.polys))) : Vec3{};
    return *id;
}

/// Every polygon edge and every off-mesh link that leaves this tile, as an abstract edge.
Status NavHierarchy::connect_tile_edges(const NavMesh& mesh, u32 slot, u32 entry,
                                        HierarchyUpdate& update) noexcept {
    for (u32 poly_index = 0; poly_index < tiles_[entry].region_of_poly.size(); ++poly_index) {
        const RegionId here = tiles_[entry].region_of_poly[poly_index];
        if (here == kInvalidRegion) {
            continue;
        }
        const PolyRef ref = mesh.tile_poly(slot, poly_index);
        for (const PolyRef neighbour : mesh.poly_neighbours(ref)) {
            if (!neighbour.valid() || neighbour.tile() == slot) {
                continue;
            }
            const RegionId there = region_of(neighbour, mesh);
            if (there == kInvalidRegion) {
                continue;
            }
            const f32 cost = region_distance(regions_[here].region, regions_[there].region);
            if (Status connected = connect(here, there, cost, update); !connected) {
                return connected;
            }
        }
        for (const LinkId link_id : mesh.links_from(ref)) {
            const NavLink* link = mesh.link(link_id);
            if (link == nullptr) {
                continue;
            }
            const PolyRef far = (link->from_poly == ref) ? link->to_poly : link->from_poly;
            const RegionId there = region_of(far, mesh);
            if (there == kInvalidRegion) {
                continue;
            }
            if (Status connected = connect(here, there, link->cost, update); !connected) {
                return connected;
            }
        }
    }
    return ok();
}

Expected<HierarchyUpdate, Error> NavHierarchy::rebuild_tile(const NavMesh& mesh,
                                                            TileCoord coord) noexcept {
    HierarchyUpdate update;
    const u32 slot = mesh.tile_slot(coord);
    if (slot == 0xFFFFFFFFu) {
        return make_unexpected(
            Error{ErrorCode::NotFound, "no tile is resident at that coordinate"});
    }

    const Expected<u32, Error> entry_index = acquire_tile(coord);
    if (!entry_index) {
        return make_unexpected(entry_index.error());
    }
    drop_regions_of(*entry_index, update);

    const u32 poly_count = mesh.tile_poly_count(slot);
    if (Status sized = tiles_[*entry_index].region_of_poly.resize(poly_count); !sized) {
        return make_unexpected(sized.error());
    }
    for (RegionId& owner : tiles_[*entry_index].region_of_poly.span()) {
        owner = kInvalidRegion;
    }

    u32 component = 0;
    for (u32 seed = 0; seed < poly_count; ++seed) {
        if (tiles_[*entry_index].region_of_poly[seed] != kInvalidRegion) {
            continue;
        }
        if (Expected<RegionId, Error> region =
                flood_region(mesh, slot, *entry_index, seed, component, update);
            !region) {
            return make_unexpected(region.error());
        }
        ++component;
    }

    if (Status connected = connect_tile_edges(mesh, slot, *entry_index, update); !connected) {
        return make_unexpected(connected.error());
    }
    tiles_[*entry_index].salt = mesh.version();
    ++rebuilds_;
    return update;
}

Status NavHierarchy::release_tile(TileCoord coord) noexcept {
    const u32 entry = tile_entry(coord);
    if (entry == 0xFFFFFFFFu) {
        return make_unexpected(Error{ErrorCode::NotFound, "the hierarchy has no such tile"});
    }
    for (const RegionId id : tiles_[entry].regions.span()) {
        if (id < regions_.size() && regions_[id].live) {
            regions_[id].region.resident = false;
        }
    }
    return ok();
}

Status NavHierarchy::forget_tile(TileCoord coord) noexcept {
    const u32 entry = tile_entry(coord);
    if (entry == 0xFFFFFFFFu) {
        return make_unexpected(Error{ErrorCode::NotFound, "the hierarchy has no such tile"});
    }
    HierarchyUpdate discarded;
    drop_regions_of(entry, discarded);
    tiles_[entry].live = false;
    return ok();
}

u32 NavHierarchy::region_count() const noexcept {
    return static_cast<u32>(regions_.size());
}

const NavRegion* NavHierarchy::region(RegionId id) const noexcept {
    if (id >= regions_.size() || !regions_[id].live) {
        return nullptr;
    }
    return &regions_[id].region;
}

RegionId NavHierarchy::region_of(PolyRef ref, const NavMesh& mesh) const noexcept {
    if (!ref.valid()) {
        return kInvalidRegion;
    }
    const TileCoord coord = mesh.tile_coord(ref.tile());
    const u32 entry = tile_entry(coord);
    if (entry == 0xFFFFFFFFu || ref.poly() >= tiles_[entry].region_of_poly.size()) {
        return kInvalidRegion;
    }
    return tiles_[entry].region_of_poly[ref.poly()];
}

RegionId NavHierarchy::region_at(const NavMesh& mesh, Vec3 position, Vec3 extents) const noexcept {
    Vec3 nearest;
    const PolyRef ref = mesh.find_nearest(position, extents, kAllAreas, nearest);
    return region_of(ref, mesh);
}

Span<const RegionEdge> NavHierarchy::edges_of(RegionId id) const noexcept {
    if (id >= regions_.size() || !regions_[id].live) {
        return {};
    }
    return regions_[id].edges.span();
}

RegionId NavHierarchy::cheapest_open(Span<const f32> best, Span<const u8> open,
                                     Vec3 goal) const noexcept {
    RegionId current = kInvalidRegion;
    f32 current_score = math::kInfinity;
    for (RegionId id = 0; id < regions_.size(); ++id) {
        // OPEN ONLY. A closed node carries `2` and must not be selected again: selecting it would
        // close it a second time, pick it a third, and never terminate.
        if (open[id] != 1) {
            continue;
        }
        const f32 score = best[id] + length(goal - regions_[id].region.centre);
        if (score < current_score - 1e-6F) {
            current_score = score;
            current = id;
        }
    }
    return current;
}

/// A*, over at most a few hundred regions. An array-backed open set rather than a heap: the scan is
/// cheaper than the heap's bookkeeping at this size, and the tie-break is the region id, which is
/// what makes two runs of one scenario produce one plan.
bool NavHierarchy::search(RegionId from, RegionId to, Array<f32>& best, Array<RegionId>& came_from,
                          Array<u8>& open) const noexcept {
    best[from] = 0.0F;
    open[from] = 1;
    const Vec3 goal = regions_[to].region.centre;
    while (true) {
        const RegionId current = cheapest_open(best.span(), open.span(), goal);
        if (current == kInvalidRegion) {
            return false;
        }
        if (current == to) {
            return true;
        }
        open[current] = 2;
        for (const RegionEdge& edge : regions_[current].edges.span()) {
            const f32 candidate = best[current] + edge.cost;
            if (candidate + 1e-6F < best[edge.to]) {
                best[edge.to] = candidate;
                came_from[edge.to] = current;
                // Re-opened even if it was closed. An off-mesh link may cost less than the straight
                // line between the regions it joins, which makes the Euclidean heuristic
                // inadmissible there — and A* without re-expansion answers a longer plan when that
                // happens. Re-expansion terminates because every improvement strictly lowers
                // `best`, and `best` is bounded below by zero.
                open[edge.to] = 1;
            }
        }
    }
}

Expected<AbstractPath, Error> NavHierarchy::plan(RegionId from, RegionId to) const noexcept {
    AbstractPath path(regions_.allocator());
    if (from >= regions_.size() || to >= regions_.size() || !regions_[from].live ||
        !regions_[to].live) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a plan endpoint names no live region"});
    }
    if (from == to) {
        if (Status pushed = path.regions.push_back(from); !pushed) {
            return make_unexpected(pushed.error());
        }
        path.found = true;
        path.complete = regions_[from].region.resident;
        path.first_unresident = path.complete ? 1u : 0u;
        return path;
    }

    Array<f32> best(regions_.allocator());
    Array<RegionId> came_from(regions_.allocator());
    Array<u8> open(regions_.allocator());
    if (Status sized = best.resize(regions_.size()); !sized) {
        return make_unexpected(sized.error());
    }
    if (Status sized = came_from.resize(regions_.size()); !sized) {
        return make_unexpected(sized.error());
    }
    if (Status sized = open.resize(regions_.size()); !sized) {
        return make_unexpected(sized.error());
    }
    for (usize index = 0; index < regions_.size(); ++index) {
        best[index] = math::kInfinity;
        came_from[index] = kInvalidRegion;
        open[index] = 0;
    }

    if (!search(from, to, best, came_from, open)) {
        return path;
    }

    // Walk the predecessors back, then reverse in place. The bound is belt and braces: a cycle in
    // `came_from` cannot happen while every improvement lowers `best`, and an unbounded loop here
    // would be an out-of-memory rather than a diagnostic if it ever did.
    usize steps = 0;
    for (RegionId step = to; step != kInvalidRegion && steps <= regions_.size();
         step = came_from[step], ++steps) {
        if (Status pushed = path.regions.push_back(step); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (step == from) {
            break;
        }
    }
    if (path.regions.empty() || path.regions[path.regions.size() - 1] != from) {
        return make_unexpected(Error{ErrorCode::Internal,
                                     "the abstract search reached the goal but its predecessor "
                                     "chain does not lead back to the start"});
    }
    for (usize head = 0, tail = path.regions.size() - 1; head < tail; ++head, --tail) {
        const RegionId swap = path.regions[head];
        path.regions[head] = path.regions[tail];
        path.regions[tail] = swap;
    }
    path.found = true;
    path.cost = best[to];
    path.complete = true;
    path.first_unresident = static_cast<u32>(path.regions.size());
    for (usize index = 0; index < path.regions.size(); ++index) {
        if (!regions_[path.regions[index]].region.resident) {
            path.complete = false;
            path.first_unresident = static_cast<u32>(index);
            break;
        }
    }
    return path;
}

Expected<PathResult, Error> NavHierarchy::refine(const NavMesh& mesh, const AbstractPath& path,
                                                 u32 segment, Vec3 from, Vec3 goal,
                                                 const PathFilter& filter,
                                                 PathCorridor& corridor) const noexcept {
    if (!path.found || segment >= path.regions.size()) {
        return make_unexpected(
            Error{ErrorCode::OutOfRange, "that segment is not part of this abstract path"});
    }
    const RegionId here = path.regions[segment];
    if (here >= regions_.size() || !regions_[here].live || !regions_[here].region.resident) {
        // `navigation`: refinement is DEFERRED until the region is resident, not failed.
        return make_unexpected(Error{ErrorCode::Unavailable,
                                     "that segment's region is not resident; the abstract path "
                                     "stands and refinement waits for the tile to stream in"});
    }
    const bool last = (segment + 1) == path.regions.size();
    Vec3 target = goal;
    if (!last) {
        const RegionId next = path.regions[segment + 1];
        if (next >= regions_.size() || !regions_[next].live) {
            return make_unexpected(
                Error{ErrorCode::NotFound, "the next region of this path no longer exists"});
        }
        if (!regions_[next].region.resident) {
            return make_unexpected(Error{ErrorCode::Unavailable,
                                         "the next region is not resident; refinement of this "
                                         "segment waits for the tile to stream in"});
        }
        target = regions_[next].region.centre;
    }
    const Vec3 extents{regions_[here].region.bounds.size().x + 1.0F, 4.0F,
                       regions_[here].region.bounds.size().z + 1.0F};
    return find_path(mesh, from, target, extents, filter, corridor);
}

NavHierarchy::Stats NavHierarchy::stats() const noexcept {
    Stats out;
    out.rebuilds = rebuilds_;
    for (const RegionEntry& entry : regions_.span()) {
        if (!entry.live) {
            continue;
        }
        ++out.regions;
        out.resident_regions += entry.region.resident ? 1u : 0u;
        out.edges += static_cast<u32>(entry.edges.size());
    }
    return out;
}

}  // namespace cy::navigation
