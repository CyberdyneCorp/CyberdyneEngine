#pragma once
// DynamicBvh and Bvh<T> — the two bounding volume hierarchies. Task 3.1.4.
//
// `core-math` — "Spatial acceleration structures":
//
//   * `DynamicBvh` — incremental, self-balancing, with **fat AABBs**, supporting insert, update,
//     remove, AABB query, frustum query and ray query. Render culling and broadphase-style queries
//     use it. **An object that moves within its expanded AABB does not modify the tree** — that is
//     the scenario the margin exists for, and it is what makes a tree of ten thousand moving
//     objects affordable.
//   * `Bvh<T>` — static, built with a surface-area heuristic, for triangle meshes and baked data.
//
// The two are separate types rather than one with a flag because their costs are opposite. The
// dynamic tree is optimised for cheap incremental change and accepts a worse tree; the static one
// spends real time at build to produce the best tree it can, and cannot be modified afterwards.
//
// QUERIES ARE CONSERVATIVE, for the reason `Frustum` documents: they may hand the callback a proxy
// that does not truly intersect, and never omit one that does. A dynamic tree is *doubly*
// conservative, because it stores fat bounds — the callback receives proxies whose real bounds may
// be up to `margin` outside the query. A caller that needs exactness re-tests with the real bounds,
// which it has and the tree does not.

#include <cy/core/base/assert.h>
#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/geometry.h>
#include <cy/core/math/scalar.h>
#include <cy/core/math/shapes.h>

#include <algorithm>
#include <type_traits>
#include <utility>
#include <vector>

namespace cy {

/// The index that means "no node". Not 0: node 0 is a perfectly ordinary node, and a null sentinel
/// that is also a valid index is a bug waiting for the tree to be non-empty.
inline constexpr u32 kNullBvhNode = 0xFFFFFFFFu;

/// The traversal stack's depth. A binary tree of 2^128 leaves does not fit in memory, so this
/// cannot be exceeded by a well-formed tree; it is asserted rather than grown because a traversal
/// that reallocates is a traversal that allocates on the render thread.
inline constexpr usize kBvhStackDepth = 128;

/// An incremental, self-balancing bounding volume hierarchy over fat AABBs.
///
/// Proxies are stable: the `u32` returned by `insert` names the same object until it is removed,
/// and is not reused before then. A removed proxy's index may be handed out again by a later
/// insert, so a caller that keeps proxies across removals needs its own generation — that is what
/// `cy::Handle` in the values module is for, and duplicating it here would be a second, different
/// generation scheme.
class DynamicBvh {
public:
    struct Node {
        /// The **fat** bounds: the object's real bounds grown by the tree's margin. The tree never
        /// sees the real bounds, which is exactly why a small movement costs nothing.
        Aabb bounds;
        u64 user_data = 0;
        u32 parent = kNullBvhNode;
        u32 child1 = kNullBvhNode;
        u32 child2 = kNullBvhNode;
        /// Height above the leaves; -1 marks a node on the free list. A leaf is 0.
        i32 height = -1;

        [[nodiscard]] bool is_leaf() const noexcept { return child1 == kNullBvhNode; }
    };

    /// `margin` is in metres and is added on every axis. 0.1 m suits a scene measured in metres:
    /// large enough that walking pace does not restructure the tree every frame, small enough that
    /// the fattened boxes do not overlap each other into uselessness.
    explicit DynamicBvh(f32 margin = 0.1f) noexcept : margin_(margin) {}

    /// Insert `bounds`, fattened by the margin. `user_data` is carried untouched and handed back by
    /// every query — an entity id, a mesh index, whatever the caller needs to identify the object.
    [[nodiscard]] Expected<u32, Error> insert(const Aabb& bounds, u64 user_data);

    [[nodiscard]] Expected<void, Error> remove(u32 proxy);

    /// Move a proxy. Returns **true when the tree was restructured** and false when the new bounds
    /// still fit inside the stored fat bounds and nothing had to change.
    ///
    /// That return value is the scenario "small movement does not restructure" made observable: a
    /// test asserts it is false for a movement inside the margin, which is a stronger check than
    /// asserting that the tree still answers queries correctly.
    [[nodiscard]] Expected<bool, Error> update(u32 proxy, const Aabb& bounds);

    [[nodiscard]] const Aabb& fat_bounds(u32 proxy) const noexcept { return nodes_[proxy].bounds; }
    [[nodiscard]] u64 user_data(u32 proxy) const noexcept { return nodes_[proxy].user_data; }
    [[nodiscard]] usize size() const noexcept { return leaf_count_; }
    [[nodiscard]] bool empty() const noexcept { return leaf_count_ == 0; }
    [[nodiscard]] f32 margin() const noexcept { return margin_; }
    [[nodiscard]] u32 root() const noexcept { return root_; }

    /// The height of the tree. A perfectly balanced tree of n leaves has height ceil(log2(n)); this
    /// is what a diagnostic compares against to say whether the balancing is working.
    [[nodiscard]] i32 height() const noexcept {
        return root_ == kNullBvhNode ? 0 : nodes_[root_].height;
    }

    /// The sum of internal node surface areas over the root's surface area — the standard measure
    /// of BVH quality. Lower is better; a well-built tree over uniformly distributed objects sits
    /// somewhere near 1.5 to 3.
    [[nodiscard]] f32 surface_area_ratio() const noexcept;

    void clear() noexcept;

    // --- Queries
    // -----------------------------------------------------------------------------------
    //
    // Templated on the callback so it inlines: a query that called through a function pointer would
    // spend more time on the indirect call than on the box test. `fn(u32 proxy, u64 user_data)`
    // returning `void` visits everything; returning `bool` stops the traversal on false.

    template <class Fn>
    void query_aabb(const Aabb& box, Fn&& fn) const {
        traverse(std::forward<Fn>(fn),
                 [&box](const Aabb& node_bounds) { return box.intersects(node_bounds); });
    }

    template <class Fn>
    void query_frustum(const Frustum& frustum, Fn&& fn) const {
        traverse(std::forward<Fn>(fn),
                 [&frustum](const Aabb& node_bounds) { return frustum.intersects(node_bounds); });
    }

    /// Visits every leaf whose fat bounds the ray enters within `max_distance`, in no particular
    /// order. Nearest-hit callers narrow `max_distance` themselves as they find hits; the tree does
    /// not do it for them because it does not know what "a hit" means to the caller.
    template <class Fn>
    void query_ray(const Ray& ray, f32 max_distance, Fn&& fn) const {
        traverse(std::forward<Fn>(fn), [&ray, max_distance](const Aabb& node_bounds) {
            f32 t_min = 0.0f;
            f32 t_max = 0.0f;
            return geom::ray_aabb(ray, node_bounds, max_distance, t_min, t_max);
        });
    }

private:
    /// Depth-first with an explicit stack. Recursion would be shorter and would put an unbounded
    /// frame count on a thread whose stack the job system sizes.
    template <class Fn, class Test>
    void traverse(Fn&& fn, Test&& test) const {
        if (root_ == kNullBvhNode) {
            return;
        }
        u32 stack[kBvhStackDepth];
        usize top = 0;
        stack[top++] = root_;
        while (top > 0) {
            const u32 index = stack[--top];
            const Node& node = nodes_[index];
            if (!test(node.bounds)) {
                continue;
            }
            if (node.is_leaf()) {
                if constexpr (std::is_same_v<decltype(fn(index, node.user_data)), bool>) {
                    if (!fn(index, node.user_data)) {
                        return;
                    }
                } else {
                    fn(index, node.user_data);
                }
                continue;
            }
            CY_ASSERT_MSG(top + 2 <= kBvhStackDepth, "DynamicBvh traversal stack overflow");
            stack[top++] = node.child1;
            stack[top++] = node.child2;
        }
    }

    [[nodiscard]] Expected<u32, Error> allocate_node();
    void free_node(u32 index) noexcept;
    void insert_leaf(u32 leaf);
    void remove_leaf(u32 leaf) noexcept;
    /// One rotation at `index` if it improves balance; returns the new subtree root.
    [[nodiscard]] u32 balance(u32 index) noexcept;
    /// The sibling an insertion of `leaf_bounds` should be paired with, chosen by descending
    /// toward whichever child costs less in surface area.
    [[nodiscard]] u32 best_sibling(const Aabb& leaf_bounds) const noexcept;

    std::vector<Node> nodes_;
    u32 root_ = kNullBvhNode;
    u32 free_list_ = kNullBvhNode;
    usize leaf_count_ = 0;
    f32 margin_ = 0.1f;
};

/// A static bounding volume hierarchy built with a binned surface-area heuristic.
///
/// For triangle meshes and baked data: built once, queried many times, never modified. `T` is
/// whatever the caller wants back from a query — a triangle index, a submesh id, a pointer it owns.
/// It is stored by value in a flat array parallel to the leaf order, so `T` should be small and
/// trivially copyable; it is a payload, not an owner.
template <class T>
class Bvh {
public:
    struct Node {
        Aabb bounds;
        /// A leaf's first primitive, or an internal node's left child.
        u32 first = 0;
        /// An internal node's right child. Stored rather than derived: the builder emits the whole
        /// left subtree before the right child, so the two children are not adjacent and a
        /// "right = left + 1" shortcut would silently address a grandchild.
        u32 second = 0;
        /// 0 for an internal node; the primitive count for a leaf.
        u32 count = 0;

        [[nodiscard]] bool is_leaf() const noexcept { return count != 0; }
    };

    /// Build over `count` primitives. `bounds[i]` and `payloads[i]` describe primitive i; the
    /// builder reorders them internally and `payload(index)` reads back in the tree's order.
    ///
    /// `max_leaf_size` is where the recursion stops. Larger leaves make a smaller, cheaper tree and
    /// more work per leaf; 4 is the usual balance for triangles.
    [[nodiscard]] Expected<void, Error> build(const Aabb* bounds, const T* payloads, usize count,
                                              u32 max_leaf_size = 4);

    [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }
    [[nodiscard]] usize size() const noexcept { return payloads_.size(); }
    [[nodiscard]] usize node_count() const noexcept { return nodes_.size(); }
    [[nodiscard]] const Aabb& bounds() const noexcept { return nodes_[0].bounds; }
    [[nodiscard]] const T& payload(usize index) const noexcept { return payloads_[index]; }

    void clear() noexcept {
        nodes_.clear();
        payloads_.clear();
        primitive_bounds_.clear();
    }

    /// `fn(const T& payload, const Aabb& primitive_bounds)`; returning `bool` stops on false.
    template <class Fn>
    void query_aabb(const Aabb& box, Fn&& fn) const {
        traverse(std::forward<Fn>(fn),
                 [&box](const Aabb& node_bounds) { return box.intersects(node_bounds); });
    }

    template <class Fn>
    void query_frustum(const Frustum& frustum, Fn&& fn) const {
        traverse(std::forward<Fn>(fn),
                 [&frustum](const Aabb& node_bounds) { return frustum.intersects(node_bounds); });
    }

    template <class Fn>
    void query_ray(const Ray& ray, f32 max_distance, Fn&& fn) const {
        traverse(std::forward<Fn>(fn), [&ray, max_distance](const Aabb& node_bounds) {
            f32 t_min = 0.0f;
            f32 t_max = 0.0f;
            return geom::ray_aabb(ray, node_bounds, max_distance, t_min, t_max);
        });
    }

private:
    /// Hand every primitive of one leaf to the callback. Returns false when the callback asked to
    /// stop, which is the only way `traverse` ends early.
    ///
    /// Separate from `traverse` so that the `if constexpr` distinguishing a `bool`-returning
    /// callback from a `void`-returning one sits in a three-line function rather than inside two
    /// levels of loop. It is the same code either way; it is just readable here.
    template <class Fn>
    [[nodiscard]] bool visit_leaf(const Node& node, Fn&& fn) const {
        for (u32 i = 0; i < node.count; ++i) {
            const u32 index = node.first + i;
            if constexpr (std::is_same_v<decltype(fn(payloads_[index], primitive_bounds_[index])),
                                         bool>) {
                if (!fn(payloads_[index], primitive_bounds_[index])) {
                    return false;
                }
            } else {
                fn(payloads_[index], primitive_bounds_[index]);
            }
        }
        return true;
    }

    template <class Fn, class Test>
    void traverse(Fn&& fn, Test&& test) const {
        if (nodes_.empty()) {
            return;
        }
        u32 stack[kBvhStackDepth];
        usize top = 0;
        stack[top++] = 0;
        while (top > 0) {
            const Node& node = nodes_[stack[--top]];
            if (!test(node.bounds)) {
                continue;
            }
            if (node.is_leaf()) {
                if (!visit_leaf(node, fn)) {
                    return;
                }
                continue;
            }
            CY_ASSERT_MSG(top + 2 <= kBvhStackDepth, "Bvh traversal stack overflow");
            stack[top++] = node.first;
            stack[top++] = node.second;
        }
    }

    /// Where a range of primitives should be split, as a bucket index, or `kNoSplit` when no split
    /// separates them. `axis` is the one the centroids spread furthest along.
    struct SplitChoice {
        u32 bucket = 0;
        bool found = false;
    };

    /// The binned surface-area heuristic over one range. Split out of `build_range` because it is
    /// the one part of the build that is an algorithm rather than bookkeeping.
    [[nodiscard]] SplitChoice choose_split(u32 first, u32 count, u32 axis, f32 axis_min,
                                           f32 scale) const;

    /// Partition `[first, first + count)` in place so that everything at or below `split` comes
    /// first, and return the boundary. Payloads move with their bounds.
    [[nodiscard]] u32 partition_range(u32 first, u32 count, u32 axis, f32 axis_min, f32 scale,
                                      u32 split);

    /// Which of the SAH buckets primitive `index` falls in.
    [[nodiscard]] u32 bucket_of(u32 index, u32 axis, f32 axis_min, f32 scale) const;

    /// Recursive binned SAH split of `[first, first + count)`, returning the node it wrote.
    u32 build_range(u32 first, u32 count, u32 max_leaf_size);

    std::vector<Node> nodes_;
    std::vector<T> payloads_;
    std::vector<Aabb> primitive_bounds_;
};

// --- Bvh<T> implementation
// -------------------------------------------------------------------------
//
// In the header because it is a template. The dynamic tree's implementation is in src/bvh.cpp,
// where it belongs: it is not a template and it is the larger of the two.

template <class T>
Expected<void, Error> Bvh<T>::build(const Aabb* bounds, const T* payloads, usize count,
                                    u32 max_leaf_size) {
    clear();
    if (count == 0) {
        return {};
    }
    if (bounds == nullptr || payloads == nullptr) {
        return cy::fail(ErrorCode::InvalidArgument, "Bvh::build() called with a null array");
    }
    if (max_leaf_size == 0) {
        return cy::fail(ErrorCode::InvalidArgument, "Bvh::build() needs a positive max_leaf_size");
    }
    if (count > 0xFFFFFFFFull / 2ull) {
        return cy::fail(ErrorCode::OutOfRange, "Bvh::build() exceeds the 32-bit primitive index");
    }

    primitive_bounds_.assign(bounds, bounds + count);
    payloads_.assign(payloads, payloads + count);
    // Every split emits at most two nodes per primitive, so 2n is a sufficient reservation and the
    // build never reallocates mid-recursion — which matters because build_range() holds indices
    // into nodes_ across its recursive calls.
    nodes_.reserve(2 * count);
    build_range(0, static_cast<u32>(count), max_leaf_size);
    return {};
}

/// The number of SAH buckets. Twelve is the number the original binned-SAH paper found gives
/// essentially the same tree as an exact sweep at a fraction of the cost.
inline constexpr u32 kBvhBucketCount = 12;

/// The bucket index primitive `index` falls in, along `axis`.
template <class T>
u32 Bvh<T>::bucket_of(u32 index, u32 axis, f32 axis_min, f32 scale) const {
    const f32 offset = (primitive_bounds_[index].center()[axis] - axis_min) * scale;
    const i32 bucket = static_cast<i32>(offset);
    return static_cast<u32>(math::clamp(bucket, 0, static_cast<i32>(kBvhBucketCount) - 1));
}

template <class T>
Bvh<T>::SplitChoice Bvh<T>::choose_split(u32 first, u32 count, u32 axis, f32 axis_min,
                                         f32 scale) const {
    struct Bucket {
        u32 count = 0;
        Aabb bounds = Aabb::empty();
    };
    Bucket buckets[kBvhBucketCount];
    for (u32 i = 0; i < count; ++i) {
        Bucket& bucket = buckets[bucket_of(first + i, axis, axis_min, scale)];
        bucket.count += 1;
        bucket.bounds.grow(primitive_bounds_[first + i]);
    }

    // The cost of splitting after bucket b, in the usual SAH currency: the probability of entering
    // each side (its surface area) times the number of primitives that would be there.
    SplitChoice choice;
    f32 best_cost = math::kInfinity;
    for (u32 split = 0; split + 1 < kBvhBucketCount; ++split) {
        Aabb left = Aabb::empty();
        Aabb right = Aabb::empty();
        u32 left_count = 0;
        u32 right_count = 0;
        for (u32 b = 0; b <= split; ++b) {
            left.grow(buckets[b].bounds);
            left_count += buckets[b].count;
        }
        for (u32 b = split + 1; b < kBvhBucketCount; ++b) {
            right.grow(buckets[b].bounds);
            right_count += buckets[b].count;
        }
        if (left_count == 0 || right_count == 0) {
            continue;
        }
        const f32 cost = (left.surface_area() * static_cast<f32>(left_count)) +
                         (right.surface_area() * static_cast<f32>(right_count));
        if (cost < best_cost) {
            best_cost = cost;
            choice.bucket = split;
            choice.found = true;
        }
    }
    return choice;
}

template <class T>
u32 Bvh<T>::partition_range(u32 first, u32 count, u32 axis, f32 axis_min, f32 scale, u32 split) {
    u32 left = first;
    u32 right = first + count;
    while (left < right) {
        if (bucket_of(left, axis, axis_min, scale) <= split) {
            ++left;
        } else {
            --right;
            std::swap(primitive_bounds_[left], primitive_bounds_[right]);
            std::swap(payloads_[left], payloads_[right]);
        }
    }
    return left;
}

template <class T>
u32 Bvh<T>::build_range(u32 first, u32 count, u32 max_leaf_size) {
    const u32 node_index = static_cast<u32>(nodes_.size());
    nodes_.push_back(Node{});

    Aabb node_bounds = Aabb::empty();
    Aabb centroid_bounds = Aabb::empty();
    for (u32 i = 0; i < count; ++i) {
        node_bounds.grow(primitive_bounds_[first + i]);
        centroid_bounds.grow(primitive_bounds_[first + i].center());
    }
    nodes_[node_index].bounds = node_bounds;

    const auto make_leaf = [&]() noexcept {
        nodes_[node_index].first = first;
        nodes_[node_index].count = count;
        return node_index;
    };

    if (count <= max_leaf_size) {
        return make_leaf();
    }

    // Split along the axis the centroids spread furthest over. A zero spread means every centroid
    // coincides and no plane separates them: emit a leaf rather than recursing forever.
    const Vec3 extent = centroid_bounds.size();
    // The nesting is the tie-breaking, so it is spelled out rather than folded into conditional
    // operators: z wins every comparison it does not lose, which is what puts 2u in both
    // fallthrough arms.
    u32 axis = 2u;
    if (extent.x > extent.y) {
        if (extent.x > extent.z) {
            axis = 0u;
        }
    } else if (extent.y > extent.z) {
        axis = 1u;
    }
    if (extent[axis] <= 0.0f) {
        return make_leaf();
    }

    const f32 axis_min = centroid_bounds.min[axis];
    const f32 scale = static_cast<f32>(kBvhBucketCount) / extent[axis];
    const SplitChoice choice = choose_split(first, count, axis, axis_min, scale);

    u32 mid = first;
    if (choice.found) {
        mid = partition_range(first, count, axis, axis_min, scale, choice.bucket);
    }
    // A partition that put everything on one side makes no progress; the median split always makes
    // both sides smaller and so always terminates.
    if (mid == first || mid == first + count) {
        mid = first + (count / 2);
    }

    const u32 left_child = build_range(first, mid - first, max_leaf_size);
    const u32 right_child = build_range(mid, first + count - mid, max_leaf_size);
    nodes_[node_index].first = left_child;
    nodes_[node_index].second = right_child;
    nodes_[node_index].count = 0;
    return node_index;
}

}  // namespace cy
