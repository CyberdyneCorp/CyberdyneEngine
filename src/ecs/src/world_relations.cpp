// Parent-child relationships, kept consistent by the world. Task 2.9.
//
// `ecs-core`: the relation is "maintained as `Parent` and `Children` components kept consistent by
// the world rather than by user code". Everything below is the world doing that, and it is the only
// code in the engine that writes either component. Both sides of every edge change in one call, so
// there is no window in which a child names a parent whose `Children` does not name it back.

#include <cy/ecs/world.h>

namespace cy::ecs {

Entity World::parent_of(Entity entity) const noexcept {
    const auto* parent = get<Parent>(entity, parent_component_);
    return (parent == nullptr) ? kNoEntity : parent->value;
}

Span<const Entity> World::children_of(Entity entity) const noexcept {
    const void* slot = get(entity, children_component_);
    if (slot == nullptr) {
        return {};
    }
    // A read-only view over the buffer, built without the allocator a BufferView carries for
    // growth: reading children must not be able to allocate.
    //
    // NOLINTBEGIN(bugprone-casting-through-void) — through void* on purpose: the inline elements
    // begin at an offset the column layout aligned, and a direct reinterpret_cast from `u8*` is
    // what -Wcast-align reports under -Werror. buffer.h does the same, for the same reason.
    const auto* header = static_cast<const BufferHeader*>(slot);
    const auto* inline_bytes =
        static_cast<const u8*>(slot) + align_up(sizeof(BufferHeader), alignof(Entity));
    const auto* elements = (header->heap != nullptr)
                               ? static_cast<const Entity*>(header->heap)
                               : static_cast<const Entity*>(static_cast<const void*>(inline_bytes));
    // NOLINTEND(bugprone-casting-through-void)
    return {elements, header->size};
}

Status World::detach_from_parent(Entity child) noexcept {
    const Entity parent = parent_of(child);
    if (!parent.valid() || !is_alive(parent)) {
        return ok();
    }
    Expected<BufferView<Entity>, Error> children = buffer<Entity>(parent, children_component_);
    if (!children) {
        return ok();
    }
    (void)children->remove_first(child);
    return ok();
}

Status World::set_parent(Entity child, Entity parent) noexcept {
    if (!is_alive(child)) {
        return fail(ErrorCode::NotFound, "set_parent() on an entity that is not alive");
    }
    if (parent.valid() && !is_alive(parent)) {
        return fail(ErrorCode::NotFound, "set_parent() names a parent that is not alive");
    }
    if (child == parent) {
        return fail(ErrorCode::InvalidArgument, "an entity cannot be its own parent");
    }
    // A cycle would make `collect_subtree` and every traversal above it non-terminating, and the
    // walk to the root is bounded by the depth, so this is the cheapest place to refuse it.
    for (Entity ancestor = parent; ancestor.valid(); ancestor = parent_of(ancestor)) {
        if (ancestor == child) {
            return fail(ErrorCode::InvalidArgument,
                        "reparenting here would make the hierarchy a cycle");
        }
    }
    if (Status admitted = admit_structural_change(); !admitted) {
        return admitted;
    }

    if (Status detached = detach_from_parent(child); !detached) {
        return detached;
    }

    if (!parent.valid()) {
        if (has(child, parent_component_)) {
            return remove(child, parent_component_);
        }
        return ok();
    }

    if (!has(child, parent_component_)) {
        const Parent value{parent};
        if (Status added = add(child, parent_component_, &value); !added) {
            return added;
        }
    } else {
        auto* value = get_mut<Parent>(child, parent_component_);
        value->value = parent;
    }

    if (!has(parent, children_component_)) {
        if (Status added = add(parent, children_component_); !added) {
            return added;
        }
    }
    Expected<BufferView<Entity>, Error> children = buffer<Entity>(parent, children_component_);
    if (!children) {
        return make_unexpected(children.error());
    }
    return children->push_back(child);
}

Status World::collect_subtree(Entity root, Array<Entity>& out) const noexcept {
    if (Status pushed = out.push_back(root); !pushed) {
        return pushed;
    }
    // Breadth-first, so the array is ordered parents-before-children and the caller can destroy it
    // in reverse to release each buffer before the entity holding it goes away. The traversal is
    // over an acyclic hierarchy — `set_parent` refuses to make one that is not.
    for (usize index = 0; index < out.size(); ++index) {
        const Entity entity = out[index];
        for (const Entity child : children_of(entity)) {
            if (Status pushed = out.push_back(child); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

}  // namespace cy::ecs
