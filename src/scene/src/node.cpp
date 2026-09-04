// `Node` — identity, naming, hierarchy, paths, components and groups. Tasks 3.1.1, 3.1.2, 3.1.8.
//
// Every function here is a read or a write through the ECS. There is no member of `Node` to
// consult: the handle is a tree pointer and an entity id, and this file is the proof of it.

#include <cy/scene/node.h>

#include <cy/scene/tree.h>

#include <algorithm>
#include <cstdio>

namespace cy::scene {
namespace {

/// The longest name the suffixing loop can produce: an interned name plus `_` and a counter.
inline constexpr usize kNameBufferBytes = Name::kMaxLength + 16;

/// A child of `parent` whose name is `name`, ignoring `except`. The sibling-uniqueness test.
[[nodiscard]] Entity sibling_named(const SceneTree& tree, Entity parent, Name name,
                                   Entity except) noexcept {
    const World& world = tree.world();
    for (const Entity child : world.children_of(parent)) {
        if (child == except) {
            continue;
        }
        const auto* held = world.get<NodeName>(child, tree.components().node_name);
        if (held != nullptr && held->value == name) {
            return child;
        }
    }
    return ecs::kNoEntity;
}

/// `wanted`, or `wanted_2`, `wanted_3`, ... — the first spelling no sibling holds.
///
/// `scene-graph-and-nodes` asks for suffixing rather than refusal, and the reason is loading: the
/// same prefab instantiated twice under one parent must not need the caller to invent names.
[[nodiscard]] Name unique_sibling_name(const SceneTree& tree, Entity parent, Name wanted,
                                       Entity except) noexcept {
    if (!parent.valid() || sibling_named(tree, parent, wanted, except) == ecs::kNoEntity) {
        return wanted;
    }
    const std::string_view base = wanted.text();
    char buffer[kNameBufferBytes];
    // Bounded rather than unbounded: a parent cannot hold more children than the world holds
    // entities, so the loop terminates, and the bound makes that visible instead of implied.
    for (u32 suffix = 2; suffix != 0; ++suffix) {
        const int written = std::snprintf(buffer, sizeof(buffer), "%.*s_%u",
                                          static_cast<int>(base.size()), base.data(), suffix);
        if (written <= 0) {
            return wanted;
        }
        const Name candidate = Name::intern(std::string_view(buffer, static_cast<usize>(written)));
        if (sibling_named(tree, parent, candidate, except) == ecs::kNoEntity) {
            return candidate;
        }
    }
    return wanted;
}

/// This node's authored position among its siblings, or zero when it has none.
[[nodiscard]] u32 order_of(const SceneTree& tree, Entity entity) noexcept {
    const auto* order = tree.world().get<ChildOrder>(entity, tree.components().child_order);
    return (order == nullptr) ? 0U : order->value;
}

/// Split `path` at the first `/`. Returns the segment and advances `path` past the separator.
[[nodiscard]] std::string_view take_segment(std::string_view& path) noexcept {
    const usize slash = path.find('/');
    if (slash == std::string_view::npos) {
        const std::string_view segment = path;
        path = std::string_view();
        return segment;
    }
    const std::string_view segment = path.substr(0, slash);
    path.remove_prefix(slash + 1);
    return segment;
}

}  // namespace

World* Node::world() const noexcept {
    return (tree_ == nullptr) ? nullptr : &tree_->world();
}

bool Node::alive_with_node() const noexcept {
    const World& world = tree_->world();
    // Liveness is the entity table's answer, never a flag this handle carries — which is why a node
    // whose entity a system destroyed is invalid the instant the destroy lands, with no
    // invalidation pass anywhere.
    return world.is_alive(entity_) && world.has(entity_, tree_->components().node_name);
}

// --- Naming ---------------------------------------------------------------------------------

Name Node::name() const noexcept {
    if (!valid()) {
        return {};
    }
    const auto* held = tree_->world().get<NodeName>(entity_, tree_->components().node_name);
    return (held == nullptr) ? Name() : held->value;
}

Status Node::set_name(Name name) const noexcept {
    if (!valid()) {
        return fail(ErrorCode::NotFound, "set_name() on a node that is not valid");
    }
    const Name unique =
        unique_sibling_name(*tree_, tree_->world().parent_of(entity_), name, entity_);
    auto* held = tree_->world().get_mut<NodeName>(entity_, tree_->components().node_name);
    if (held == nullptr) {
        return fail(ErrorCode::NotFound, "this node has no NodeName component");
    }
    held->value = unique;
    return ok();
}

Name Node::alias() const noexcept {
    if (!valid()) {
        return {};
    }
    const auto* held = tree_->world().get<NodeAlias>(entity_, tree_->components().node_alias);
    return (held == nullptr) ? Name() : held->value;
}

Status Node::set_alias(Name alias) const noexcept {
    return tree_->set_alias(*this, alias);
}

Status Node::clear_alias() const noexcept {
    return tree_->clear_alias(*this);
}

Status Node::path(Array<char>& out) const noexcept {
    out.clear();
    if (!valid()) {
        return fail(ErrorCode::NotFound, "path() on a node that is not valid");
    }
    // Collected root-first by walking up and then reversing, because a node knows its parent and
    // not its ancestors. The tree root contributes the leading `/` and no name of its own.
    Array<Name> names(tree_->allocator());
    for (Node node = *this; node.valid() && node.entity() != tree_->root().entity();
         node = node.parent()) {
        if (Status pushed = names.push_back(node.name()); !pushed) {
            return pushed;
        }
    }
    for (usize index = names.size(); index > 0; --index) {
        const std::string_view text = names[index - 1].text();
        if (Status slash = out.push_back('/'); !slash) {
            return slash;
        }
        if (Status appended = out.append(Span<const char>(text.data(), text.size())); !appended) {
            return appended;
        }
    }
    if (out.empty()) {
        if (Status slash = out.push_back('/'); !slash) {
            return slash;
        }
    }
    return out.push_back('\0');
}

// --- Hierarchy ------------------------------------------------------------------------------

Node Node::parent() const noexcept {
    if (!valid()) {
        return {};
    }
    return {*tree_, tree_->world().parent_of(entity_)};
}

u32 Node::child_count() const noexcept {
    if (!valid()) {
        return 0;
    }
    return static_cast<u32>(tree_->world().children_of(entity_).size());
}

Node Node::child(u32 index) const noexcept {
    if (!valid()) {
        return {};
    }
    // The authored order is dense, so the `index`-th child is the one whose ChildOrder equals it.
    for (const Entity child : tree_->world().children_of(entity_)) {
        if (order_of(*tree_, child) == index) {
            return {*tree_, child};
        }
    }
    return {};
}

Node Node::child(Name name) const noexcept {
    if (!valid()) {
        return {};
    }
    const Entity found = sibling_named(*tree_, entity_, name, ecs::kNoEntity);
    return found.valid() ? Node(*tree_, found) : Node();
}

Status Node::children(Array<Node>& out) const noexcept {
    if (!valid()) {
        return fail(ErrorCode::NotFound, "children() on a node that is not valid");
    }
    const usize first = out.size();
    for (const Entity child : tree_->world().children_of(entity_)) {
        if (Status pushed = out.push_back(Node(*tree_, child)); !pushed) {
            return pushed;
        }
    }
    SceneTree& tree = *tree_;
    std::sort(out.begin() + static_cast<isize>(first), out.end(),
              [&tree](Node left, Node right) noexcept {
                  const u32 left_order = order_of(tree, left.entity());
                  const u32 right_order = order_of(tree, right.entity());
                  return (left_order != right_order)
                             ? (left_order < right_order)
                             : (left.entity().index() < right.entity().index());
              });
    return ok();
}

u32 Node::depth() const noexcept {
    u32 depth = 0;
    for (Node node = parent(); node.valid(); node = node.parent()) {
        ++depth;
    }
    return depth;
}

bool Node::is_ancestor_of(Node other) const noexcept {
    if (!valid() || !other.valid()) {
        return false;
    }
    for (Node node = other.parent(); node.valid(); node = node.parent()) {
        if (node == *this) {
            return true;
        }
    }
    return false;
}

u32 Node::sibling_index() const noexcept {
    return valid() ? order_of(*tree_, entity_) : 0U;
}

Status Node::set_sibling_index(u32 index) const noexcept {
    if (!valid()) {
        return fail(ErrorCode::NotFound, "set_sibling_index() on a node that is not valid");
    }
    Array<Node> siblings(tree_->allocator());
    Node holder = parent();
    if (!holder.valid()) {
        return fail(ErrorCode::NotFound, "a node with no parent has no siblings to order among");
    }
    if (Status collected = holder.children(siblings); !collected) {
        return collected;
    }
    const u32 clamped = std::min<u32>(index, static_cast<u32>(siblings.size()) - 1U);
    u32 next = 0;
    // Renumber in the order the caller asked for: everything else keeps its relative order and this
    // node takes the requested slot.
    for (usize pass = 0; pass < siblings.size() + 1; ++pass) {
        if (next == clamped) {
            if (auto* order =
                    tree_->world().get_mut<ChildOrder>(entity_, tree_->components().child_order);
                order != nullptr) {
                order->value = next;
            }
            ++next;
        }
        if (pass == siblings.size()) {
            break;
        }
        const Node sibling = siblings[pass];
        if (sibling == *this) {
            continue;
        }
        if (auto* order = tree_->world().get_mut<ChildOrder>(sibling.entity(),
                                                             tree_->components().child_order);
            order != nullptr) {
            order->value = next;
        }
        ++next;
    }
    return ok();
}

Node Node::resolve_segment(std::string_view segment) const noexcept {
    if (segment.empty() || segment == ".") {
        return *this;
    }
    if (segment == "..") {
        return parent();
    }
    const Name name = Name::find(segment);
    // `find` rather than `intern`: resolving a path that names nothing must not grow the process's
    // name table with keys that were only ever probed.
    return name.is_empty() ? Node() : child(name);
}

Node Node::find(std::string_view path) const noexcept {
    if (!valid()) {
        return {};
    }
    if (!path.empty() && path.front() == '/') {
        return tree_->find(path);
    }
    Node current = *this;
    while (!path.empty() && current.valid()) {
        current = current.resolve_segment(take_segment(path));
    }
    return current;
}

Status Node::add_child(Node child, Reparent mode) const noexcept {
    return child.set_parent(*this, mode);
}

Status Node::set_parent(Node parent, Reparent mode) const noexcept {
    if (!valid()) {
        return fail(ErrorCode::NotFound, "set_parent() on a node that is not valid");
    }
    const Transform kept = world_transform();
    const bool was_in_tree = in_tree();
    const Entity previous_parent = tree_->world().parent_of(entity_);

    if (Status reparented = tree_->world().set_parent(entity_, parent.entity_); !reparented) {
        return reparented;
    }

    // The old parent's numbering has a gap where this node was; closing it is what keeps
    // `child(index)` an equality test rather than a rank computation.
    if (Status renumbered = tree_->renumber_children(previous_parent); !renumbered) {
        return renumbered;
    }
    if (auto* order = tree_->world().get_mut<ChildOrder>(entity_, tree_->components().child_order);
        order != nullptr) {
        // Appended last, which is what `World::set_parent` did to the ECS buffer as well.
        order->value = parent.valid() ? (parent.child_count() - 1U) : 0U;
    }
    // The name may collide with a sibling it did not have before; suffixing here is what keeps the
    // "unique among siblings" invariant true across a reparent rather than only across a creation.
    if (Status renamed = set_name(name()); !renamed) {
        return renamed;
    }

    if (mode == Reparent::KeepWorld) {
        if (Status kept_world = set_world_transform(kept); !kept_world) {
            return kept_world;
        }
    } else if (Status marked = mark_transform_changed(); !marked) {
        return marked;
    }
    if (Status marked = mark_flags_changed(*tree_, entity_); !marked) {
        return marked;
    }

    const bool now_in_tree = in_tree();
    if (now_in_tree != was_in_tree) {
        return tree_->queue_lifecycle(entity_, now_in_tree);
    }
    return ok();
}

bool Node::in_tree() const noexcept {
    if (!valid()) {
        return false;
    }
    const Entity root = tree_->root().entity();
    for (Node node = *this; node.valid(); node = node.parent()) {
        if (node.entity() == root) {
            return true;
        }
    }
    return false;
}

// --- Components -----------------------------------------------------------------------------

bool Node::has(ComponentTypeId component) const noexcept {
    return valid() && tree_->world().has(entity_, component);
}

const void* Node::get(ComponentTypeId component) const noexcept {
    return valid() ? tree_->world().get(entity_, component) : nullptr;
}

void* Node::get_mut(ComponentTypeId component) const noexcept {
    return valid() ? tree_->world().get_mut(entity_, component) : nullptr;
}

Status Node::add(ComponentTypeId component, const void* value) const noexcept {
    if (!valid()) {
        return fail(ErrorCode::NotFound, "add() on a node that is not valid");
    }
    return tree_->world().add(entity_, component, value);
}

Status Node::remove(ComponentTypeId component) const noexcept {
    if (!valid()) {
        return fail(ErrorCode::NotFound, "remove() on a node that is not valid");
    }
    return tree_->world().remove(entity_, component);
}

// --- Groups ---------------------------------------------------------------------------------

Status Node::add_to_group(Name group) const noexcept {
    if (!valid()) {
        return fail(ErrorCode::NotFound, "add_to_group() on a node that is not valid");
    }
    Expected<ComponentTypeId, Error> tag = tree_->groups().ensure(*world(), group);
    if (!tag) {
        return make_unexpected(tag.error());
    }
    if (tree_->world().has(entity_, *tag)) {
        return ok();
    }
    return tree_->world().add(entity_, *tag);
}

Status Node::remove_from_group(Name group) const noexcept {
    if (!valid()) {
        return fail(ErrorCode::NotFound, "remove_from_group() on a node that is not valid");
    }
    const ComponentTypeId tag = tree_->groups().find(group);
    if (tag == kInvalidComponent || !tree_->world().has(entity_, tag)) {
        return ok();
    }
    return tree_->world().remove(entity_, tag);
}

bool Node::in_group(Name group) const noexcept {
    if (!valid()) {
        return false;
    }
    const ComponentTypeId tag = tree_->groups().find(group);
    return tag != kInvalidComponent && tree_->world().has(entity_, tag);
}

Status Node::destroy() const noexcept {
    return tree_->destroy_node(*this);
}

}  // namespace cy::scene
