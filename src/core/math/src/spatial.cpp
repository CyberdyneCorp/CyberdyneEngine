// SpatialHash and Octree. Task 3.1.4. See include/cy/core/math/spatial.h.

#include <cy/core/math/spatial.h>

#include <cy/core/base/assert.h>

#include <algorithm>
#include <cmath>

namespace cy {

// --- SpatialHash
// ------------------------------------------------------------------------------------

IVec3 SpatialHash::cell_of(Vec3 position) const noexcept {
    const f32 inv = 1.0f / cell_size_;
    return IVec3{static_cast<i32>(std::floor(position.x * inv)),
                 static_cast<i32>(std::floor(position.y * inv)),
                 static_cast<i32>(std::floor(position.z * inv))};
}

u64 SpatialHash::cell_key(IVec3 cell) noexcept {
    const u64 x = static_cast<u64>(static_cast<u32>(cell.x)) & 0x1FFFFFull;
    const u64 y = static_cast<u64>(static_cast<u32>(cell.y)) & 0x1FFFFFull;
    const u64 z = static_cast<u64>(static_cast<u32>(cell.z)) & 0x1FFFFFull;
    return (x << 42) | (y << 21) | z;
}

void SpatialHash::insert_into_cells(u32 slot, const Aabb& bounds) {
    const IVec3 lo = cell_of(bounds.min);
    const IVec3 hi = cell_of(bounds.max);
    for (i32 z = lo.z; z <= hi.z; ++z) {
        for (i32 y = lo.y; y <= hi.y; ++y) {
            for (i32 x = lo.x; x <= hi.x; ++x) {
                cells_[cell_key(IVec3{x, y, z})].push_back(slot);
            }
        }
    }
}

void SpatialHash::remove_from_cells(u32 slot, const Aabb& bounds) {
    const IVec3 lo = cell_of(bounds.min);
    const IVec3 hi = cell_of(bounds.max);
    for (i32 z = lo.z; z <= hi.z; ++z) {
        for (i32 y = lo.y; y <= hi.y; ++y) {
            for (i32 x = lo.x; x <= hi.x; ++x) {
                const u64 key = cell_key(IVec3{x, y, z});
                const auto it = cells_.find(key);
                if (it == cells_.end()) {
                    continue;
                }
                std::vector<u32>& list = it->second;
                // Swap-and-pop: the order within a cell carries no meaning, so there is no reason
                // to pay for preserving it.
                const auto found = std::find(list.begin(), list.end(), slot);
                if (found != list.end()) {
                    *found = list.back();
                    list.pop_back();
                }
                if (list.empty()) {
                    cells_.erase(it);
                }
            }
        }
    }
}

Expected<void, Error> SpatialHash::insert(u32 id, const Aabb& bounds) {
    if (bounds.is_empty()) {
        return cy::fail(ErrorCode::InvalidArgument, "SpatialHash::insert(): an empty bounding box");
    }
    if (cell_size_ <= 0.0f) {
        return cy::fail(ErrorCode::InvalidArgument, "SpatialHash: the cell size must be positive");
    }

    const auto existing = id_to_slot_.find(id);
    if (existing != id_to_slot_.end()) {
        const u32 slot = existing->second;
        remove_from_cells(slot, entries_[slot].bounds);
        entries_[slot].bounds = bounds;
        insert_into_cells(slot, bounds);
        return {};
    }

    u32 slot = 0;
    if (!free_slots_.empty()) {
        slot = free_slots_.back();
        free_slots_.pop_back();
    } else {
        slot = static_cast<u32>(entries_.size());
        entries_.push_back(Entry{});
        visit_stamp_.push_back(0);
    }
    entries_[slot] = Entry{bounds, id, true};
    id_to_slot_[id] = slot;
    insert_into_cells(slot, bounds);
    return {};
}

Expected<void, Error> SpatialHash::update(u32 id, const Aabb& bounds) {
    if (id_to_slot_.find(id) == id_to_slot_.end()) {
        return cy::fail(ErrorCode::NotFound, "SpatialHash::update(): no such id");
    }
    return insert(id, bounds);
}

Expected<void, Error> SpatialHash::remove(u32 id) {
    const auto existing = id_to_slot_.find(id);
    if (existing == id_to_slot_.end()) {
        return cy::fail(ErrorCode::NotFound, "SpatialHash::remove(): no such id");
    }
    const u32 slot = existing->second;
    remove_from_cells(slot, entries_[slot].bounds);
    entries_[slot].alive = false;
    free_slots_.push_back(slot);
    id_to_slot_.erase(existing);
    return {};
}

void SpatialHash::clear() noexcept {
    entries_.clear();
    free_slots_.clear();
    id_to_slot_.clear();
    cells_.clear();
    visit_stamp_.clear();
    query_stamp_ = 0;
}

// --- Octree
// -------------------------------------------------------------------------------------------

Aabb Octree::octant_bounds(const Aabb& parent, u32 octant) noexcept {
    const Vec3 center = parent.center();
    // The same bit order as `Aabb::corner`: bit 0 is the +X half, bit 1 the +Y half, bit 2 the +Z.
    return Aabb{Vec3{(octant & 1u) != 0u ? center.x : parent.min.x,
                     (octant & 2u) != 0u ? center.y : parent.min.y,
                     (octant & 4u) != 0u ? center.z : parent.min.z},
                Vec3{(octant & 1u) != 0u ? parent.max.x : center.x,
                     (octant & 2u) != 0u ? parent.max.y : center.y,
                     (octant & 4u) != 0u ? parent.max.z : center.z}};
}

Expected<void, Error> Octree::reset(const Aabb& bounds, u32 max_depth, u32 split_threshold) {
    if (bounds.is_empty()) {
        return cy::fail(ErrorCode::InvalidArgument, "Octree::reset(): an empty root volume");
    }
    if (max_depth == 0 || max_depth > 16) {
        return cy::fail(ErrorCode::InvalidArgument,
                        "Octree::reset(): max_depth must be in [1, 16]");
    }
    if (split_threshold == 0) {
        return cy::fail(ErrorCode::InvalidArgument,
                        "Octree::reset(): split_threshold must be positive");
    }
    clear();
    root_bounds_ = bounds;
    max_depth_ = max_depth;
    split_threshold_ = split_threshold;
    nodes_.push_back(Node{});
    nodes_[0].bounds = bounds;
    nodes_[0].depth = 0;
    return {};
}

void Octree::subdivide(u32 node) {
    // Children are created all eight at once: a half-subdivided node would need the descent to
    // handle "this octant exists and that one does not" on every step, and eight empty vectors are
    // cheaper than that branch.
    const Aabb parent_bounds = nodes_[node].bounds;
    const u32 depth = nodes_[node].depth;
    for (u32 octant = 0; octant < 8; ++octant) {
        const u32 child = static_cast<u32>(nodes_.size());
        nodes_.push_back(Node{});
        nodes_[child].bounds = octant_bounds(parent_bounds, octant);
        nodes_[child].depth = depth + 1;
        // `nodes_` may have reallocated inside push_back, so the parent is addressed by index
        // again rather than through a reference taken before the loop.
        nodes_[node].children[octant] = child;
    }
    nodes_[node].has_children = true;
}

u32 Octree::descend(u32 node, const Aabb& bounds) {
    u32 current = node;
    for (;;) {
        if (nodes_[current].depth >= max_depth_) {
            return current;
        }
        if (!nodes_[current].has_children) {
            if (nodes_[current].entries.size() < split_threshold_) {
                return current;
            }
            subdivide(current);
        }
        // The entry goes into a child only if that child wholly contains it. An entry straddling a
        // split plane stays here, which is what keeps it in exactly one node.
        u32 chosen = kNoChild;
        for (const u32 child : nodes_[current].children) {
            if (nodes_[child].bounds.contains(bounds)) {
                chosen = child;
                break;
            }
        }
        if (chosen == kNoChild) {
            return current;
        }
        current = chosen;
    }
}

Expected<void, Error> Octree::insert(u32 id, const Aabb& bounds) {
    if (nodes_.empty()) {
        return cy::fail(ErrorCode::Unavailable, "Octree::insert(): reset() has not been called");
    }
    if (bounds.is_empty()) {
        return cy::fail(ErrorCode::InvalidArgument, "Octree::insert(): an empty bounding box");
    }
    if (!root_bounds_.contains(bounds)) {
        // The tree's volume is fixed, so an entry outside it has nowhere to go. Reporting rather
        // than clamping: an object that has left the world is a fact the caller needs, not
        // something to hide by pinning it to the boundary.
        return cy::fail(ErrorCode::OutOfRange,
                        "Octree::insert(): the bounds lie outside the tree's root volume");
    }

    const auto existing = id_to_node_.find(id);
    if (existing != id_to_node_.end()) {
        const Expected<void, Error> removed = remove(id);
        if (!removed) {
            return removed;
        }
    }

    const u32 node = descend(0, bounds);
    nodes_[node].entries.push_back(Entry{bounds, id});
    id_to_node_[id] = node;
    return {};
}

Expected<void, Error> Octree::remove(u32 id) {
    const auto existing = id_to_node_.find(id);
    if (existing == id_to_node_.end()) {
        return cy::fail(ErrorCode::NotFound, "Octree::remove(): no such id");
    }
    std::vector<Entry>& entries = nodes_[existing->second].entries;
    for (usize i = 0; i < entries.size(); ++i) {
        if (entries[i].id == id) {
            entries[i] = entries.back();
            entries.pop_back();
            break;
        }
    }
    id_to_node_.erase(existing);
    return {};
}

Expected<void, Error> Octree::update(u32 id, const Aabb& bounds) {
    if (id_to_node_.find(id) == id_to_node_.end()) {
        return cy::fail(ErrorCode::NotFound, "Octree::update(): no such id");
    }
    return insert(id, bounds);
}

void Octree::clear() noexcept {
    nodes_.clear();
    id_to_node_.clear();
}

}  // namespace cy
