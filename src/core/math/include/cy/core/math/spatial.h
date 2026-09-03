#pragma once
// SpatialHash and Octree — the two non-hierarchical acceleration structures. Task 3.1.4.
//
// `core-math` — "Spatial acceleration structures":
//
//   * `SpatialHash` — a uniform grid, for evenly distributed dynamic objects. It has no build step
//     and no rebalancing; insertion and removal are O(cells covered), which for objects near the
//     cell size is a small constant. It degrades badly when the objects are not evenly sized: one
//     object spanning a thousand cells occupies a thousand cell lists.
//   * `Octree` — a regular subdivision, for volumetric queries where that shape is preferable to a
//     BVH's: anything that asks "what is in this region of space" rather than "what does this
//     object touch", and anything that wants a node's extent to be a known power-of-two fraction of
//     the root.
//
// Neither replaces `DynamicBvh` (bvh.h), and the choice between the three is about the *object
// distribution*, not about taste. A `SpatialHash` beats a BVH for a crowd of similar agents; a BVH
// beats it for a scene mixing a coin and a mountain; an octree beats both when the query is a
// region rather than a bound.
//
// IDS ARE THE CALLER'S. Both structures store a `u32` the caller chose and hand it back; neither
// invents identity. Two entries with the same id are the same entry — inserting an id that is
// already present replaces it, which is what makes `update` expressible as an insert.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>

#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cy {

/// A uniform grid over an unbounded space, hashed rather than allocated.
///
/// The grid has no extent: a cell exists once something is in it. That is what makes it usable for
/// a world whose bounds are not known, and it is why the cell size is the only tuning parameter
/// that matters — it should be near the size of a typical object, so that a typical insertion
/// touches one to eight cells.
class SpatialHash {
public:
    explicit SpatialHash(f32 cell_size = 1.0f) noexcept : cell_size_(cell_size) {}

    /// Insert or replace `id`'s bounds.
    [[nodiscard]] Expected<void, Error> insert(u32 id, const Aabb& bounds);

    /// Move an entry. Equivalent to `insert`, and named separately because the call site reads
    /// better and because it reports `ErrorCode::NotFound` for an id that was never inserted, which
    /// `insert` cannot.
    [[nodiscard]] Expected<void, Error> update(u32 id, const Aabb& bounds);

    [[nodiscard]] Expected<void, Error> remove(u32 id);

    [[nodiscard]] bool contains(u32 id) const noexcept { return id_to_slot_.count(id) != 0; }
    [[nodiscard]] usize size() const noexcept { return id_to_slot_.size(); }
    [[nodiscard]] usize cell_count() const noexcept { return cells_.size(); }
    [[nodiscard]] f32 cell_size() const noexcept { return cell_size_; }

    void clear() noexcept;

    /// Visit every entry whose bounds intersect `box`, each **exactly once** even when it occupies
    /// several of the cells the query touches.
    ///
    /// The de-duplication is a per-query stamp rather than a set: a set would allocate on the
    /// query, and a query that allocates is a query that cannot run inside a job. `fn(u32 id, const
    /// Aabb& bounds)`; returning `bool` stops the traversal on false.
    template <class Fn>
    void query_aabb(const Aabb& box, Fn&& fn) const {
        if (entries_.empty() || box.is_empty()) {
            return;
        }
        ++query_stamp_;
        const IVec3 lo = cell_of(box.min);
        const IVec3 hi = cell_of(box.max);
        for (i32 z = lo.z; z <= hi.z; ++z) {
            for (i32 y = lo.y; y <= hi.y; ++y) {
                for (i32 x = lo.x; x <= hi.x; ++x) {
                    const auto it = cells_.find(cell_key(IVec3{x, y, z}));
                    if (it == cells_.end()) {
                        continue;
                    }
                    for (const u32 slot : it->second) {
                        if (visit_stamp_[slot] == query_stamp_) {
                            continue;
                        }
                        visit_stamp_[slot] = query_stamp_;
                        const Entry& entry = entries_[slot];
                        if (!entry.bounds.intersects(box)) {
                            continue;
                        }
                        if constexpr (std::is_same_v<decltype(fn(entry.id, entry.bounds)), bool>) {
                            if (!fn(entry.id, entry.bounds)) {
                                return;
                            }
                        } else {
                            fn(entry.id, entry.bounds);
                        }
                    }
                }
            }
        }
    }

    /// The cell a world position falls in. Public because a caller sometimes wants to reason about
    /// the grid directly — a debug draw, a spatial partitioning of work across jobs.
    [[nodiscard]] IVec3 cell_of(Vec3 position) const noexcept;

private:
    struct Entry {
        Aabb bounds;
        u32 id = 0;
        bool alive = false;
    };

    /// Fold a cell coordinate into a 64-bit key. Three 21-bit fields rather than a hash: it is
    /// exact for coordinates in ±2^20 cells, which at a one-metre cell is a million metres from the
    /// origin — well beyond where 32-bit float positions stop being useful anyway (`core-math` —
    /// "Precision"). An exact key means no collision handling and no comparison of cell
    /// coordinates on lookup.
    [[nodiscard]] static u64 cell_key(IVec3 cell) noexcept;

    void insert_into_cells(u32 slot, const Aabb& bounds);
    void remove_from_cells(u32 slot, const Aabb& bounds);

    f32 cell_size_ = 1.0f;
    std::vector<Entry> entries_;
    std::vector<u32> free_slots_;
    std::unordered_map<u32, u32> id_to_slot_;
    std::unordered_map<u64, std::vector<u32>> cells_;

    /// Mutable because a query is logically const and still has to record what it has seen. Grown
    /// alongside `entries_`, so it is indexed by slot.
    mutable std::vector<u64> visit_stamp_;
    mutable u64 query_stamp_ = 0;
};

/// A regular octree over a fixed root volume.
///
/// An entry lives in the deepest node that wholly contains it, so an object straddling a split
/// plane stays at the parent rather than being duplicated into both children. That keeps every
/// entry in exactly one place — insertion, removal and de-duplication all become trivial — at the
/// cost of large straddling objects sitting near the root and being visited by most queries. It is
/// the right trade for a tree used for volumetric queries; a tree used for broadphase would want
/// the loose variant, and that is what `DynamicBvh` is for.
class Octree {
public:
    Octree() = default;

    /// Set the root volume and the subdivision limits, discarding any contents.
    ///
    /// `max_depth` bounds the memory: a full tree is 8^depth nodes, so 8 is already 16 million and
    /// nothing should need more. `split_threshold` is how many entries a node holds before it
    /// subdivides.
    [[nodiscard]] Expected<void, Error> reset(const Aabb& bounds, u32 max_depth = 8,
                                              u32 split_threshold = 8);

    [[nodiscard]] Expected<void, Error> insert(u32 id, const Aabb& bounds);
    [[nodiscard]] Expected<void, Error> remove(u32 id);
    [[nodiscard]] Expected<void, Error> update(u32 id, const Aabb& bounds);

    [[nodiscard]] bool contains(u32 id) const noexcept { return id_to_node_.count(id) != 0; }
    [[nodiscard]] usize size() const noexcept { return id_to_node_.size(); }
    [[nodiscard]] usize node_count() const noexcept { return nodes_.size(); }
    [[nodiscard]] const Aabb& bounds() const noexcept { return root_bounds_; }

    void clear() noexcept;

    /// `fn(u32 id, const Aabb& bounds)`; returning `bool` stops on false.
    template <class Fn>
    void query_aabb(const Aabb& box, Fn&& fn) const {
        traverse(
            std::forward<Fn>(fn),
            [&box](const Aabb& node_bounds) { return box.intersects(node_bounds); },
            [&box](const Aabb& entry_bounds) { return box.intersects(entry_bounds); });
    }

    /// Conservative in the frustum's own sense (shapes.h): the node test may accept a node that is
    /// outside, and the per-entry test is the same conservative AABB test.
    template <class Fn>
    void query_frustum(const Frustum& frustum, Fn&& fn) const {
        traverse(
            std::forward<Fn>(fn),
            [&frustum](const Aabb& node_bounds) { return frustum.intersects(node_bounds); },
            [&frustum](const Aabb& entry_bounds) { return frustum.intersects(entry_bounds); });
    }

private:
    /// "No child in this octant." Declared before `Node` so that its default member initialiser
    /// reads without the reader having to know the complete-class rules.
    static constexpr u32 kNoChild = 0xFFFFFFFFu;

    struct Entry {
        Aabb bounds;
        u32 id = 0;
    };

    struct Node {
        Aabb bounds;
        /// The eight children, or `kNoChild` where the node has not subdivided in that octant.
        u32 children[8] = {kNoChild, kNoChild, kNoChild, kNoChild,
                           kNoChild, kNoChild, kNoChild, kNoChild};
        std::vector<Entry> entries;
        u32 depth = 0;
        bool has_children = false;
    };

    template <class Fn, class NodeTest, class EntryTest>
    void traverse(Fn&& fn, NodeTest&& node_test, EntryTest&& entry_test) const {
        if (nodes_.empty()) {
            return;
        }
        // The depth bound makes the stack bound: a node pushes at most eight children, and the
        // tree is at most max_depth_ deep.
        std::vector<u32> stack;
        stack.push_back(0);
        while (!stack.empty()) {
            const Node& node = nodes_[stack.back()];
            stack.pop_back();
            if (!node_test(node.bounds)) {
                continue;
            }
            for (const Entry& entry : node.entries) {
                if (!entry_test(entry.bounds)) {
                    continue;
                }
                if constexpr (std::is_same_v<decltype(fn(entry.id, entry.bounds)), bool>) {
                    if (!fn(entry.id, entry.bounds)) {
                        return;
                    }
                } else {
                    fn(entry.id, entry.bounds);
                }
            }
            if (!node.has_children) {
                continue;
            }
            for (const u32 child : node.children) {
                if (child != kNoChild) {
                    stack.push_back(child);
                }
            }
        }
    }

    /// The deepest existing-or-created node that wholly contains `bounds`, starting at `node`.
    [[nodiscard]] u32 descend(u32 node, const Aabb& bounds);
    void subdivide(u32 node);
    [[nodiscard]] static Aabb octant_bounds(const Aabb& parent, u32 octant) noexcept;

    std::vector<Node> nodes_;
    std::unordered_map<u32, u32> id_to_node_;
    Aabb root_bounds_ = Aabb::empty();
    u32 max_depth_ = 8;
    u32 split_threshold_ = 8;
};

}  // namespace cy
