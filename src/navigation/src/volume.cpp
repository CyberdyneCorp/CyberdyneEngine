// Sparse-voxel paths and representation-neutral deterministic query delivery.

#include <cy/navigation/volume.h>

#include <algorithm>
#include <cmath>

namespace cy::navigation {
namespace {

constexpr i32 kCoordMin = -(1 << 20);
constexpr i32 kCoordMax = (1 << 20) - 1;
constexpr u32 kNoParent = 0xFFFFFFFFU;

[[nodiscard]] bool valid(VolumeCoord coord) noexcept {
    return coord.x >= kCoordMin && coord.x <= kCoordMax && coord.y >= kCoordMin &&
           coord.y <= kCoordMax && coord.z >= kCoordMin && coord.z <= kCoordMax;
}

[[nodiscard]] u64 key_of(VolumeCoord coord) noexcept {
    return (static_cast<u64>(coord.x - kCoordMin) << 42U) |
           (static_cast<u64>(coord.y - kCoordMin) << 21U) | static_cast<u64>(coord.z - kCoordMin);
}

[[nodiscard]] bool coord_at(Vec3 point, f32 cell_size, VolumeCoord& coord) noexcept {
    const f32 limit = static_cast<f32>(-kCoordMin) * cell_size;
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) ||
        point.x < -limit || point.y < -limit || point.z < -limit || point.x >= limit ||
        point.y >= limit || point.z >= limit) {
        return false;
    }
    coord = VolumeCoord{static_cast<i32>(std::floor(point.x / cell_size)),
                        static_cast<i32>(std::floor(point.y / cell_size)),
                        static_cast<i32>(std::floor(point.z / cell_size))};
    return valid(coord);
}

[[nodiscard]] f32 heuristic(Vec3 a, Vec3 b) noexcept {
    return length(a - b);
}

struct SearchNode {
    VolumeCoord coord;
    f32 g = 0.0F;
    f32 h = 0.0F;
    u32 parent = kNoParent;
};

struct WorseFirst {
    const Array<SearchNode>* nodes = nullptr;

    [[nodiscard]] bool operator()(u32 a, u32 b) const noexcept {
        const SearchNode& left = (*nodes)[a];
        const SearchNode& right = (*nodes)[b];
        const f32 left_f = left.g + left.h;
        const f32 right_f = right.g + right.h;
        return left_f != right_f ? left_f > right_f : key_of(left.coord) > key_of(right.coord);
    }
};

struct Search {
    Array<SearchNode> nodes;
    Array<u32> open;
    Array<bool> closed;
    HashMap<u64, u32> indexes;
    u32 best = 0;

    explicit Search(Allocator& allocator) noexcept
        : nodes(allocator), open(allocator), closed(allocator), indexes(allocator) {}
};

[[nodiscard]] bool better_partial(const SearchNode& candidate, const SearchNode& current) noexcept {
    return candidate.h < current.h ||
           (candidate.h == current.h && key_of(candidate.coord) < key_of(current.coord));
}

[[nodiscard]] bool add_start(Search& search, VolumeCoord start, Vec3 goal,
                             const NavVolume& volume) noexcept {
    SearchNode first;
    first.coord = start;
    first.h = heuristic(volume.center(start), goal);
    return search.nodes.push_back(first).has_value() &&
           search.closed.push_back(false).has_value() &&
           search.indexes.insert(key_of(start), 0).has_value() &&
           search.open.push_back(0).has_value();
}

[[nodiscard]] bool relax(Search& search, u32 current, VolumeCoord next, f32 tentative, Vec3 goal,
                         const NavVolume& volume, PathResult& result) noexcept {
    const u64 key = key_of(next);
    if (u32* found = search.indexes.find(key); found != nullptr) {
        if (tentative >= search.nodes[*found].g) {
            return true;
        }
        SearchNode& node = search.nodes[*found];
        node.g = tentative;
        node.parent = current;
        search.closed[*found] = false;
        if (!search.open.push_back(*found)) {
            return false;
        }
        std::ranges::push_heap(search.open.span(), WorseFirst{&search.nodes});
        if (better_partial(node, search.nodes[search.best])) {
            search.best = *found;
        }
        return true;
    }

    SearchNode node;
    node.coord = next;
    node.g = tentative;
    node.h = heuristic(volume.center(next), goal);
    node.parent = current;
    if (!search.nodes.push_back(node) || !search.closed.push_back(false)) {
        return false;
    }
    const u32 index = static_cast<u32>(search.nodes.size() - 1);
    if (!search.indexes.insert(key, index) || !search.open.push_back(index)) {
        return false;
    }
    std::ranges::push_heap(search.open.span(), WorseFirst{&search.nodes});
    ++result.nodes_visited;
    if (better_partial(node, search.nodes[search.best])) {
        search.best = index;
    }
    return true;
}

[[nodiscard]] bool expand(Search& search, u32 current, Vec3 goal, const NavVolume& volume,
                          const PathFilter& filter, PathResult& result) noexcept {
    constexpr VolumeCoord offsets[6] = {{-1, 0, 0}, {0, -1, 0}, {0, 0, -1},
                                        {0, 0, 1},  {0, 1, 0},  {1, 0, 0}};
    const VolumeCoord from = search.nodes[current].coord;
    for (VolumeCoord offset : offsets) {
        const VolumeCoord next{from.x + offset.x, from.y + offset.y, from.z + offset.z};
        if (!valid(next)) {
            continue;
        }
        const VolumeCell* cell = volume.cell(next);
        if (cell == nullptr || (area_bit(cell->area) & filter.areas) == 0) {
            continue;
        }
        const f32 step = volume.cell_size() * cell->cost * filter.costs.of(cell->area);
        if (!relax(search, current, next, search.nodes[current].g + step, goal, volume, result)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool reconstruct(const Search& search, u32 tail, Vec3 start, Vec3 end, bool reached,
                               const NavVolume& volume, Array<Vec3>& points) noexcept {
    Array<Vec3> reverse(volume.allocator());
    for (u32 index = tail; index != kNoParent; index = search.nodes[index].parent) {
        if (!reverse.push_back(volume.center(search.nodes[index].coord))) {
            return false;
        }
    }
    points.clear();
    if (!points.push_back(start)) {
        return false;
    }
    for (usize index = reverse.size(); index > 0; --index) {
        if (!points.push_back(reverse[index - 1])) {
            return false;
        }
    }
    return !reached || points.push_back(end).has_value();
}

}  // namespace

NavVolume::NavVolume(Allocator& allocator, f32 cell_size) noexcept
    : allocator_(&allocator), cells_(allocator), cell_size_(cell_size) {}

Status NavVolume::set_cell(VolumeCoord coord, AreaType area, f32 cost) noexcept {
    if (!valid(coord) || !std::isfinite(cell_size_) || cell_size_ <= 0.0F || area >= kAreaCount ||
        area == kAreaNull || !std::isfinite(cost) || cost <= 0.0F) {
        return fail(ErrorCode::InvalidArgument, "navigation volume cell is invalid");
    }
    if (!cells_.insert(key_of(coord), VolumeCell{coord, area, cost})) {
        return fail(ErrorCode::OutOfMemory, "navigation volume cell allocation failed");
    }
    ++version_;
    return ok();
}

Status NavVolume::remove_cell(VolumeCoord coord) noexcept {
    if (!valid(coord) || !cells_.remove(key_of(coord))) {
        return fail(ErrorCode::NotFound, "navigation volume cell is absent");
    }
    ++version_;
    return ok();
}

const VolumeCell* NavVolume::cell(VolumeCoord coord) const noexcept {
    return valid(coord) ? cells_.find(key_of(coord)) : nullptr;
}

Vec3 NavVolume::center(VolumeCoord coord) const noexcept {
    return Vec3{(static_cast<f32>(coord.x) + 0.5F) * cell_size_,
                (static_cast<f32>(coord.y) + 0.5F) * cell_size_,
                (static_cast<f32>(coord.z) + 0.5F) * cell_size_};
}

bool NavVolume::nearest(Vec3 position, Vec3 extents, AreaMask areas,
                        VolumeCoord& coord) const noexcept {
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
        !std::isfinite(extents.x) || !std::isfinite(extents.y) || !std::isfinite(extents.z) ||
        extents.x < 0.0F || extents.y < 0.0F || extents.z < 0.0F) {
        return false;
    }
    bool found = false;
    f32 best_distance = math::kInfinity;
    u64 best_key = 0;
    for (const auto& entry : cells_) {
        const VolumeCell& cell = entry.value;
        if ((area_bit(cell.area) & areas) == 0) {
            continue;
        }
        const Vec3 middle = center(cell.coord);
        const Vec3 half{cell_size_ * 0.5F, cell_size_ * 0.5F, cell_size_ * 0.5F};
        const Vec3 closest{std::clamp(position.x, middle.x - half.x, middle.x + half.x),
                           std::clamp(position.y, middle.y - half.y, middle.y + half.y),
                           std::clamp(position.z, middle.z - half.z, middle.z + half.z)};
        const Vec3 delta = closest - position;
        if (std::fabs(delta.x) > extents.x || std::fabs(delta.y) > extents.y ||
            std::fabs(delta.z) > extents.z) {
            continue;
        }
        const f32 distance = length_squared(delta);
        if (!found || distance < best_distance ||
            (distance == best_distance && entry.key < best_key)) {
            found = true;
            best_distance = distance;
            best_key = entry.key;
            coord = cell.coord;
        }
    }
    return found;
}

PathResult NavVolume::find_path(Vec3 start, Vec3 end, Vec3 extents, const PathFilter& filter,
                                Array<Vec3>& points) const noexcept {
    PathResult result;
    points.clear();
    VolumeCoord source;
    if (!nearest(start, extents, filter.areas, source)) {
        return result;
    }
    VolumeCoord destination;
    const bool has_destination = coord_at(end, cell_size_, destination) &&
                                 cell(destination) != nullptr &&
                                 (area_bit(cell(destination)->area) & filter.areas) != 0;
    Search search(allocator());
    if (!add_start(search, source, end, *this)) {
        return result;
    }
    bool reached = false;
    u32 tail = 0;
    while (!search.open.empty()) {
        if (result.nodes_expanded >= filter.node_budget) {
            result.budget_exceeded = true;
            break;
        }
        std::ranges::pop_heap(search.open.span(), WorseFirst{&search.nodes});
        const u32 current = search.open[search.open.size() - 1];
        search.open.pop_back();
        if (search.closed[current]) {
            continue;
        }
        search.closed[current] = true;
        ++result.nodes_expanded;
        if (has_destination && search.nodes[current].coord == destination) {
            reached = true;
            tail = current;
            break;
        }
        if (!expand(search, current, end, *this, filter, result)) {
            return PathResult{};
        }
    }
    tail = reached ? tail : search.best;
    result.found = reconstruct(search, tail, start, end, reached, *this, points);
    result.partial = !reached;
    result.cost = search.nodes[tail].g;
    return result;
}

Allocator& NavigationSpace::allocator() const noexcept {
    return volume_ == nullptr ? mesh_->allocator() : volume_->allocator();
}

PathResult NavigationSpace::find_path(Vec3 start, Vec3 end, Vec3 extents, const PathFilter& filter,
                                      Array<Vec3>& points) const noexcept {
    if (volume_ != nullptr) {
        return volume_->find_path(start, end, extents, filter, points);
    }
    PathCorridor corridor(mesh_->allocator());
    PathResult result = navigation::find_path(*mesh_, start, end, extents, filter, corridor);
    points.clear();
    if (!result.found) {
        return result;
    }
    Vec3 path_end = end;
    if (result.partial && !corridor.empty()) {
        const NavPoly* reachable = mesh_->poly(corridor.polys()[corridor.size() - 1]);
        if (reachable != nullptr) {
            path_end = reachable->centre;
        }
    }
    Array<PathPoint> path(mesh_->allocator());
    if (!navigation::straighten(*mesh_, corridor, start, path_end, path)) {
        result.found = false;
        return result;
    }
    for (const PathPoint& point : path.span()) {
        if (!points.push_back(point.position)) {
            points.clear();
            result.found = false;
            return result;
        }
    }
    return result;
}

SpatialPathQueue::SpatialPathQueue(Allocator& allocator, NavigationSpace space,
                                   u32 latency_ticks) noexcept
    : space_(space),
      entries_(allocator),
      completed_(allocator),
      latency_(latency_ticks == 0 ? 1 : latency_ticks) {}

Expected<QueryId, Error> SpatialPathQueue::submit(u64 owner, Vec3 start, Vec3 end, Vec3 extents,
                                                  const PathFilter& filter, u32 tick) noexcept {
    Expected<Entry*, Error> entry = entries_.emplace_back(entries_.allocator());
    if (!entry) {
        return make_unexpected(entry.error());
    }
    (*entry)->owner = owner;
    (*entry)->start = start;
    (*entry)->end = end;
    (*entry)->extents = extents;
    (*entry)->filter = filter;
    (*entry)->deliver_tick = tick + latency_;
    return static_cast<QueryId>(entries_.size() - 1);
}

Status SpatialPathQueue::cancel(QueryId id) noexcept {
    if (id >= entries_.size()) {
        return fail(ErrorCode::NotFound, "no such spatial path query");
    }
    if (entries_[id].state == QueryState::Pending) {
        entries_[id].state = QueryState::Cancelled;
    }
    return ok();
}

u32 SpatialPathQueue::update(u32 tick) noexcept {
    completed_.clear();
    for (usize index = 0; index < entries_.size(); ++index) {
        Entry& entry = entries_[index];
        if (entry.state != QueryState::Pending || entry.deliver_tick > tick) {
            continue;
        }
        entry.result =
            space_.find_path(entry.start, entry.end, entry.extents, entry.filter, entry.points);
        entry.state = QueryState::Ready;
        if (!completed_.push_back(static_cast<QueryId>(index))) {
            break;
        }
    }
    return static_cast<u32>(completed_.size());
}

bool SpatialPathQueue::consume(QueryId id, Array<Vec3>& points, PathResult& result) noexcept {
    if (id >= entries_.size() || entries_[id].state != QueryState::Ready) {
        return false;
    }
    points = std::move(entries_[id].points);
    result = entries_[id].result;
    entries_[id].state = QueryState::Consumed;
    return true;
}

QueryState SpatialPathQueue::state(QueryId id) const noexcept {
    return id < entries_.size() ? entries_[id].state : QueryState::Cancelled;
}

u64 SpatialPathQueue::owner(QueryId id) const noexcept {
    return id < entries_.size() ? entries_[id].owner : 0;
}

u32 SpatialPathQueue::pending() const noexcept {
    u32 count = 0;
    for (const Entry& entry : entries_.span()) {
        count += entry.state == QueryState::Pending ? 1U : 0U;
    }
    return count;
}

}  // namespace cy::navigation
