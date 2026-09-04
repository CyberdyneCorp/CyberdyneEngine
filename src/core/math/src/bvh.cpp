// DynamicBvh. Task 3.1.4. See include/cy/core/math/bvh.h.
//
// The structure is the one Box2D's dynamic tree established and every engine has since copied: a
// flat node array with a free list, insertion by surface-area descent, and an AVL-style rotation on
// the way back up. It is written out here rather than taken as a dependency because the tree is
// twenty lines of real logic wrapped in bookkeeping, and the bookkeeping is where the bugs are —
// having it in the engine's own style, with the engine's own error type, is worth more than the
// lines it saves.

#include <cy/core/math/bvh.h>

namespace cy {
namespace {

/// Refit `bounds` and `height` at `index` from its two children. Both are derived quantities and
/// both must be updated on every path that changes a child, which is exactly the kind of pairing a
/// helper exists to keep together.
void refit(std::vector<DynamicBvh::Node>& nodes, u32 index) noexcept {
    const u32 child1 = nodes[index].child1;
    const u32 child2 = nodes[index].child2;
    nodes[index].height = 1 + math::max(nodes[child1].height, nodes[child2].height);
    nodes[index].bounds = merge(nodes[child1].bounds, nodes[child2].bounds);
}

}  // namespace

Expected<u32, Error> DynamicBvh::allocate_node() {
    if (free_list_ != kNullBvhNode) {
        const u32 index = free_list_;
        // A free node's `parent` is the next link in the free list; it carries no tree meaning.
        free_list_ = nodes_[index].parent;
        nodes_[index] = Node{};
        return index;
    }
    if (nodes_.size() >= static_cast<usize>(kNullBvhNode) - 1) {
        return cy::fail(ErrorCode::OutOfRange, "DynamicBvh: the 32-bit node index is exhausted");
    }
    nodes_.push_back(Node{});
    return static_cast<u32>(nodes_.size() - 1);
}

void DynamicBvh::free_node(u32 index) noexcept {
    nodes_[index].parent = free_list_;
    nodes_[index].height = -1;
    free_list_ = index;
}

u32 DynamicBvh::best_sibling(const Aabb& leaf_bounds) const noexcept {
    u32 index = root_;
    while (!nodes_[index].is_leaf()) {
        const u32 child1 = nodes_[index].child1;
        const u32 child2 = nodes_[index].child2;

        const f32 area = nodes_[index].bounds.surface_area();
        const Aabb combined = merge(nodes_[index].bounds, leaf_bounds);
        const f32 combined_area = combined.surface_area();

        // The cost of making the leaf a sibling of this node, and the cost every ancestor pays for
        // the enlargement, in the surface-area currency the SAH is stated in.
        const f32 cost = 2.0f * combined_area;
        const f32 inheritance = 2.0f * (combined_area - area);

        const auto descent_cost = [&](u32 child) noexcept {
            const Aabb child_combined = merge(leaf_bounds, nodes_[child].bounds);
            if (nodes_[child].is_leaf()) {
                return child_combined.surface_area() + inheritance;
            }
            // For an internal child, only the *growth* of its bounds is charged: whatever is
            // already inside it is paid for whether the leaf descends there or not.
            const f32 old_area = nodes_[child].bounds.surface_area();
            return (child_combined.surface_area() - old_area) + inheritance;
        };

        const f32 cost1 = descent_cost(child1);
        const f32 cost2 = descent_cost(child2);

        if (cost < cost1 && cost < cost2) {
            break;
        }
        index = cost1 < cost2 ? child1 : child2;
    }
    return index;
}

void DynamicBvh::insert_leaf(u32 leaf) {
    if (root_ == kNullBvhNode) {
        root_ = leaf;
        nodes_[leaf].parent = kNullBvhNode;
        return;
    }

    const u32 sibling = best_sibling(nodes_[leaf].bounds);
    const u32 old_parent = nodes_[sibling].parent;

    // The caller guaranteed a spare node by allocating one before calling; take it now.
    const Expected<u32, Error> allocated = allocate_node();
    CY_ASSERT_MSG(allocated.has_value(), "DynamicBvh::insert_leaf(): no spare node was reserved");
    const u32 new_parent = *allocated;

    nodes_[new_parent].parent = old_parent;
    nodes_[new_parent].user_data = 0;
    nodes_[new_parent].bounds = merge(nodes_[leaf].bounds, nodes_[sibling].bounds);
    nodes_[new_parent].height = nodes_[sibling].height + 1;
    nodes_[new_parent].child1 = sibling;
    nodes_[new_parent].child2 = leaf;
    nodes_[sibling].parent = new_parent;
    nodes_[leaf].parent = new_parent;

    if (old_parent != kNullBvhNode) {
        if (nodes_[old_parent].child1 == sibling) {
            nodes_[old_parent].child1 = new_parent;
        } else {
            nodes_[old_parent].child2 = new_parent;
        }
    } else {
        root_ = new_parent;
    }

    // Refit and rebalance from the new parent to the root. Balancing on the way up is what keeps
    // the tree's height logarithmic under an insertion order that is not random — and an insertion
    // order in a game never is, because objects are spawned in spatial groups.
    u32 index = nodes_[leaf].parent;
    while (index != kNullBvhNode) {
        index = balance(index);
        refit(nodes_, index);
        index = nodes_[index].parent;
    }
}

void DynamicBvh::remove_leaf(u32 leaf) noexcept {
    if (leaf == root_) {
        root_ = kNullBvhNode;
        return;
    }

    const u32 parent = nodes_[leaf].parent;
    const u32 grandparent = nodes_[parent].parent;
    const u32 sibling =
        nodes_[parent].child1 == leaf ? nodes_[parent].child2 : nodes_[parent].child1;

    if (grandparent == kNullBvhNode) {
        root_ = sibling;
        nodes_[sibling].parent = kNullBvhNode;
        free_node(parent);
        return;
    }

    // The leaf's parent had exactly two children; with one gone the parent is redundant, so the
    // sibling takes its place and the parent node is recycled.
    if (nodes_[grandparent].child1 == parent) {
        nodes_[grandparent].child1 = sibling;
    } else {
        nodes_[grandparent].child2 = sibling;
    }
    nodes_[sibling].parent = grandparent;
    free_node(parent);

    u32 index = grandparent;
    while (index != kNullBvhNode) {
        index = balance(index);
        refit(nodes_, index);
        index = nodes_[index].parent;
    }
}

u32 DynamicBvh::balance(u32 index) noexcept {
    // An AVL rotation over the subtree rooted at `index`: if one child is more than one level
    // deeper than the other, pull that child up. A height difference of one is left alone —
    // rebalancing it would cost more than the query time it saves.
    if (nodes_[index].is_leaf() || nodes_[index].height < 2) {
        return index;
    }

    const u32 left = nodes_[index].child1;
    const u32 right = nodes_[index].child2;
    const i32 difference = nodes_[right].height - nodes_[left].height;

    // `deep` is the child to promote and `shallow` the one that stays under `index`.
    if (difference > 1 || difference < -1) {
        const bool promote_right = difference > 1;
        const u32 deep = promote_right ? right : left;
        const u32 shallow = promote_right ? left : right;
        const u32 deep_child1 = nodes_[deep].child1;
        const u32 deep_child2 = nodes_[deep].child2;

        // Swap `index` and `deep`: `deep` becomes the subtree root and `index` becomes its child.
        nodes_[deep].child1 = index;
        nodes_[deep].parent = nodes_[index].parent;
        nodes_[index].parent = deep;

        if (nodes_[deep].parent != kNullBvhNode) {
            if (nodes_[nodes_[deep].parent].child1 == index) {
                nodes_[nodes_[deep].parent].child1 = deep;
            } else {
                nodes_[nodes_[deep].parent].child2 = deep;
            }
        } else {
            root_ = deep;
        }

        // Of `deep`'s two children, the taller stays with `deep` and the shorter moves under
        // `index`, which is what actually reduces the height difference.
        const bool keep_first = nodes_[deep_child1].height > nodes_[deep_child2].height;
        const u32 stays = keep_first ? deep_child1 : deep_child2;
        const u32 moves = keep_first ? deep_child2 : deep_child1;

        nodes_[deep].child2 = stays;
        if (promote_right) {
            nodes_[index].child2 = moves;
        } else {
            nodes_[index].child1 = moves;
        }
        nodes_[moves].parent = index;

        nodes_[index].bounds = merge(nodes_[shallow].bounds, nodes_[moves].bounds);
        nodes_[deep].bounds = merge(nodes_[index].bounds, nodes_[stays].bounds);
        nodes_[index].height = 1 + math::max(nodes_[shallow].height, nodes_[moves].height);
        nodes_[deep].height = 1 + math::max(nodes_[index].height, nodes_[stays].height);
        return deep;
    }

    return index;
}

Expected<u32, Error> DynamicBvh::insert(const Aabb& bounds, u64 user_data) {
    if (bounds.is_empty()) {
        return cy::fail(ErrorCode::InvalidArgument, "DynamicBvh::insert(): an empty bounding box");
    }

    // Not const: `return leaf_node` is the error path, and a const local is copied where a
    // non-const one is moved.
    Expected<u32, Error> leaf_node = allocate_node();
    if (!leaf_node) {
        return leaf_node;
    }
    const u32 leaf = *leaf_node;
    nodes_[leaf].bounds = bounds.expanded(margin_);
    nodes_[leaf].user_data = user_data;
    nodes_[leaf].height = 0;
    nodes_[leaf].child1 = kNullBvhNode;
    nodes_[leaf].child2 = kNullBvhNode;

    // insert_leaf() needs one internal node when the tree is not empty. Reserving it here, where
    // the failure can still be reported, is what lets insert_leaf() be `void` and unable to leave
    // the tree half-linked.
    if (root_ != kNullBvhNode) {
        const Expected<u32, Error> spare = allocate_node();
        if (!spare) {
            free_node(leaf);
            return spare;
        }
        free_node(*spare);
    }

    insert_leaf(leaf);
    ++leaf_count_;
    return leaf;
}

Expected<void, Error> DynamicBvh::remove(u32 proxy) {
    if (proxy >= nodes_.size() || nodes_[proxy].height != 0 || !nodes_[proxy].is_leaf()) {
        return cy::fail(ErrorCode::NotFound, "DynamicBvh::remove(): not a live proxy");
    }
    remove_leaf(proxy);
    free_node(proxy);
    --leaf_count_;
    return {};
}

Expected<bool, Error> DynamicBvh::update(u32 proxy, const Aabb& bounds) {
    if (proxy >= nodes_.size() || nodes_[proxy].height != 0 || !nodes_[proxy].is_leaf()) {
        return cy::fail(ErrorCode::NotFound, "DynamicBvh::update(): not a live proxy");
    }
    if (bounds.is_empty()) {
        return cy::fail(ErrorCode::InvalidArgument, "DynamicBvh::update(): an empty bounding box");
    }

    // THE MARGIN'S WHOLE PURPOSE. The stored bounds were fattened on insertion, so an object that
    // has moved less than the margin is still inside them and the tree does not have to change.
    // `core-math` — "Small movement does not restructure".
    if (nodes_[proxy].bounds.contains(bounds)) {
        return false;
    }

    const u64 user_data = nodes_[proxy].user_data;
    remove_leaf(proxy);
    nodes_[proxy].bounds = bounds.expanded(margin_);
    nodes_[proxy].user_data = user_data;
    nodes_[proxy].height = 0;
    nodes_[proxy].child1 = kNullBvhNode;
    nodes_[proxy].child2 = kNullBvhNode;
    nodes_[proxy].parent = kNullBvhNode;

    if (root_ != kNullBvhNode) {
        const Expected<u32, Error> spare = allocate_node();
        if (!spare) {
            return cy::make_unexpected(spare.error());
        }
        free_node(*spare);
    }
    insert_leaf(proxy);
    return true;
}

f32 DynamicBvh::surface_area_ratio() const noexcept {
    if (root_ == kNullBvhNode || nodes_[root_].is_leaf()) {
        return 0.0f;
    }
    const f32 root_area = nodes_[root_].bounds.surface_area();
    if (root_area <= 0.0f) {
        return 0.0f;
    }
    f32 total = 0.0f;
    for (const Node& node : nodes_) {
        // A free node has height -1, and a leaf contributes nothing: the measure is about how much
        // the *internal* nodes overlap, which is what a query pays for.
        if (node.height <= 0) {
            continue;
        }
        total += node.bounds.surface_area();
    }
    return total / root_area;
}

void DynamicBvh::clear() noexcept {
    nodes_.clear();
    root_ = kNullBvhNode;
    free_list_ = kNullBvhNode;
    leaf_count_ = 0;
}

}  // namespace cy
