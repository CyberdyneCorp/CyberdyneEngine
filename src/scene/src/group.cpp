// Groups as tag components. Task 3.1.8.

#include <cy/scene/group.h>

#include <cy/ecs/query.h>
#include <cy/scene/node.h>
#include <cy/scene/tree.h>

#include <algorithm>
#include <cstdio>

namespace cy::scene {
namespace {

/// The component name a group is registered under, interned so the registry can hold the pointer.
///
/// `ComponentRegistry::register_builtin` stores the `const char*` it is given, so the storage has
/// to outlive the world. `Name`'s table is process-lifetime by design (`core-type-system`), which
/// makes an interned name exactly the right thing to hand it — and gives the group's tag a name a
/// component listing shows as `cy::scene::group/enemies` rather than as an opaque number.
[[nodiscard]] Name group_component_name(Name group) noexcept {
    char buffer[Name::kMaxLength + 32];
    const std::string_view text = group.text();
    const int written = std::snprintf(buffer, sizeof(buffer), "%s%.*s", kGroupComponentPrefix,
                                      static_cast<int>(text.size()), text.data());
    if (written <= 0) {
        return {};
    }
    return Name::intern(std::string_view(buffer, static_cast<usize>(written)));
}

/// Every live entity carrying `tag`, sorted by entity index. See the header on why the sort is not
/// optional.
[[nodiscard]] Status collect_tagged(SceneTree& tree, ComponentTypeId tag,
                                    Array<Entity>& out) noexcept {
    ecs::QueryDesc desc(tree.allocator());
    if (Status with = desc.with(tag); !with) {
        return with;
    }
    ecs::Query query(tree.world(), std::move(desc));
    Status collected = ok();
    Status iterated = query.for_each_chunk([&out, &collected](ecs::QueryChunk& chunk) noexcept {
        if (!collected) {
            return;
        }
        for (const Entity entity : chunk.entities()) {
            if (Status pushed = out.push_back(entity); !pushed) {
                collected = pushed;
                return;
            }
        }
    });
    if (!iterated) {
        return iterated;
    }
    if (!collected) {
        return collected;
    }
    std::ranges::sort(
        out, [](Entity left, Entity right) noexcept { return left.index() < right.index(); });
    return ok();
}

}  // namespace

Expected<ComponentTypeId, Error> GroupRegistry::ensure(World& world, Name group) noexcept {
    if (group.is_empty()) {
        return fail(ErrorCode::InvalidArgument, "a group needs a name");
    }
    if (const ComponentTypeId* existing = ids_.find(group.index()); existing != nullptr) {
        return *existing;
    }
    const Name component_name = group_component_name(group);
    if (component_name.is_empty()) {
        return fail(ErrorCode::InvalidArgument, "this group's name is too long to register");
    }

    ecs::ComponentOptions options;
    options.kind = ecs::ComponentKind::Tag;
    Expected<ComponentTypeId, Error> tag =
        world.components().register_builtin(component_name.c_str(), 0, 1, options);
    if (!tag) {
        // The likely failure is `kMaxComponentTypes`, and it is worth reaching the caller with the
        // registry's own message: a world out of component slots because a project uses group names
        // as a per-entity attribute is a design problem, not an allocation failure.
        return make_unexpected(tag.error());
    }
    if (Status pushed = names_.push_back(group); !pushed) {
        return make_unexpected(pushed.error());
    }
    Expected<ComponentTypeId*, Error> slot = ids_.insert(group.index(), *tag);
    if (!slot) {
        names_.pop_back();
        return make_unexpected(slot.error());
    }
    return *tag;
}

ComponentTypeId GroupRegistry::find(Name group) const noexcept {
    const ComponentTypeId* found = ids_.find(group.index());
    return (found == nullptr) ? kInvalidComponent : *found;
}

Status GroupRegistry::members(SceneTree& tree, Name group, Array<Node>& out) const noexcept {
    const ComponentTypeId tag = find(group);
    if (tag == kInvalidComponent) {
        // Nothing has ever joined, so the group is empty. Not an error: asking for the members of a
        // group nobody is in is a legitimate question with the answer "none".
        return ok();
    }
    Array<Entity> entities(*allocator_);
    if (Status collected = collect_tagged(tree, tag, entities); !collected) {
        return collected;
    }
    for (const Entity entity : entities) {
        if (Status pushed = out.push_back(Node(tree, entity)); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status GroupRegistry::broadcast(SceneTree& tree, Name group, GroupVisitor visitor,
                                void* user) const noexcept {
    if (visitor == nullptr) {
        return fail(ErrorCode::InvalidArgument, "broadcast() needs a visitor");
    }
    Array<Node> nodes(*allocator_);
    if (Status collected = members(tree, group, nodes); !collected) {
        return collected;
    }
    // Collected first, then visited: the visitor is allowed to destroy the node it was handed, and
    // a broadcast that ran inside the query would be iterating a chunk it was mutating.
    for (const Node node : nodes) {
        if (node.valid()) {
            visitor(node, user);
        }
    }
    return ok();
}

Status GroupRegistry::names(Array<Name>& out) const noexcept {
    return out.append(names_.span());
}

}  // namespace cy::scene
