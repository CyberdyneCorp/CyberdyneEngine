// `SceneTree` — the node layer of one world: creation, destruction, addressing and the pump.
// Tasks 3.1.1, 3.1.2, 3.1.6.

#include <cy/scene/tree.h>

#include <cy/ecs/query.h>

#include <algorithm>
#include <cstring>

namespace cy::scene {
namespace {

/// The tree root's name. A node like any other, so that "in the tree" is a reachability question
/// with one answer rather than a second concept beside the hierarchy.
inline constexpr const char* kRootName = "/";

}  // namespace

SceneTree::SceneTree(World& world) noexcept
    : world_(&world),
      templates_(world.allocator()),
      groups_(world.allocator()),
      behaviours_(world.allocator()),
      aliases_(world.allocator()),
      pending_(world.allocator()),
      dispatching_(world.allocator()),
      scenes_(world.allocator()),
      scratch_(world.allocator()),
      scratch_children_(world.allocator()) {}

SceneTree::~SceneTree() {
    for (SceneRecord* record : scenes_) {
        record->~SceneRecord();
        allocator().deallocate(static_cast<void*>(record), sizeof(SceneRecord),
                               alignof(SceneRecord));
    }
}

Status SceneTree::initialize() noexcept {
    if (initialized_) {
        return ok();
    }
    Expected<SceneComponents, Error> ids = SceneComponents::register_all(*world_);
    if (!ids) {
        return make_unexpected(ids.error());
    }
    ids_ = *ids;
    if (Status added = templates_.add_builtins(*world_); !added) {
        return added;
    }
    Expected<Entity, Error> root =
        instantiate(Name::intern(kRootName), ecs::kNoEntity, Name(), kNoScene);
    if (!root) {
        return make_unexpected(root.error());
    }
    root_ = *root;
    initialized_ = true;
    return ok();
}

// --- Creating and destroying ------------------------------------------------------------------

Status SceneTree::collect_component_set(
    Name node_template, SceneId scene, Array<ComponentTypeId>& set,
    Span<const NodeTemplateRegistry::Binding>& bindings) noexcept {
    const ComponentTypeId base[] = {ids_.node_name,       ids_.child_order, ids_.local_transform,
                                    ids_.world_transform, ids_.flags,       ids_.state};
    if (Status appended =
            set.append(Span<const ComponentTypeId>(base, sizeof(base) / sizeof(*base)));
        !appended) {
        return appended;
    }
    if (!node_template.is_empty()) {
        Expected<Span<const NodeTemplateRegistry::Binding>, Error> bound =
            templates_.bindings_of(node_template);
        if (!bound) {
            return make_unexpected(bound.error());
        }
        bindings = *bound;
        for (const NodeTemplateRegistry::Binding& binding : bindings) {
            if (Status pushed = set.push_back(binding.component); !pushed) {
                return pushed;
            }
        }
    }
    if (scene == kNoScene) {
        return ok();
    }
    return set.push_back(ids_.scene_ref);
}

void SceneTree::write_node_defaults(Entity entity, Name name, SceneId scene) noexcept {
    *world_->get_mut<NodeName>(entity, ids_.node_name) = NodeName{name};
    *world_->get_mut<ChildOrder>(entity, ids_.child_order) = ChildOrder{0};
    *world_->get_mut<LocalTransform>(entity, ids_.local_transform) = LocalTransform{};
    *world_->get_mut<WorldTransform>(entity, ids_.world_transform) = WorldTransform{};
    *world_->get_mut<NodeFlags>(entity, ids_.flags) = NodeFlags{};
    *world_->get_mut<NodeState>(entity, ids_.state) = NodeState{kDirtyAll};
    if (scene != kNoScene) {
        *world_->get_mut<SceneRef>(entity, ids_.scene_ref) = SceneRef{scene};
    }
}

void SceneTree::apply_template_defaults(
    Entity entity, Span<const NodeTemplateRegistry::Binding> bindings) noexcept {
    for (const NodeTemplateRegistry::Binding& binding : bindings) {
        if (binding.defaults == nullptr || binding.defaults_size == 0) {
            continue;
        }
        void* slot = world_->get_mut(entity, binding.component);
        if (slot != nullptr) {
            std::memcpy(slot, binding.defaults, binding.defaults_size);
        }
    }
}

Status SceneTree::attach_template_behaviour(Entity entity, Name node_template) noexcept {
    if (node_template.is_empty()) {
        return ok();
    }
    const char* behaviour = templates_.behaviour_of(node_template);
    if (behaviour == nullptr || behaviour[0] == '\0') {
        return ok();
    }
    const BehaviourTypeId type = behaviours_.find(Name::intern(behaviour));
    if (type == kInvalidBehaviour) {
        return fail(ErrorCode::NotFound,
                    "this node template names a behaviour that is not registered");
    }
    return behaviours_.attach(*this, Node(*this, entity), type);
}

Expected<Entity, Error> SceneTree::instantiate(Name name, Entity parent, Name node_template,
                                               SceneId scene) noexcept {
    Array<ComponentTypeId> set(allocator());
    Span<const NodeTemplateRegistry::Binding> bindings;
    if (Status collected = collect_component_set(node_template, scene, set, bindings); !collected) {
        return make_unexpected(collected.error());
    }

    Expected<Entity, Error> entity = world_->create(set.span());
    if (!entity) {
        return entity;
    }
    write_node_defaults(*entity, name, scene);
    apply_template_defaults(*entity, bindings);

    // The behaviour is attached before the node is parented, so `onCreate` sees the specification's
    // "entity and components exist; parent may not be set".
    if (Status attached = attach_template_behaviour(*entity, node_template); !attached) {
        return make_unexpected(attached.error());
    }
    if (parent.valid()) {
        if (Status parented = Node(*this, *entity).set_parent(Node(*this, parent)); !parented) {
            return make_unexpected(parented.error());
        }
    }
    return *entity;
}

Expected<Node, Error> SceneTree::create_node(Name name, Node parent, Name node_template) noexcept {
    if (!initialized_) {
        return fail(ErrorCode::Unavailable, "the scene tree has not been initialized");
    }
    Expected<Entity, Error> entity = instantiate(
        name, parent.valid() ? parent.entity() : ecs::kNoEntity, node_template, kNoScene);
    if (!entity) {
        return make_unexpected(entity.error());
    }
    return Node(*this, *entity);
}

Status SceneTree::collect_subtree(Entity root, Array<Entity>& out) const noexcept {
    out.clear();
    if (Status pushed = out.push_back(root); !pushed) {
        return pushed;
    }
    // Breadth-first, so `out` is ordered parents-before-children and a child-first pass is the
    // reverse iteration. The hierarchy is acyclic: `World::set_parent` refuses to make one that is
    // not.
    for (usize index = 0; index < out.size(); ++index) {
        for (const Entity child : world_->children_of(out[index])) {
            if (Status pushed = out.push_back(child); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

Status SceneTree::destroy_node(Node node) noexcept {
    if (!node.valid()) {
        return fail(ErrorCode::NotFound, "destroy_node() on a node that is not valid");
    }
    if (node.entity() == root_) {
        return fail(ErrorCode::InvalidArgument, "the tree root cannot be destroyed");
    }
    Array<Entity> subtree(allocator());
    if (Status collected = collect_subtree(node.entity(), subtree); !collected) {
        return collected;
    }

    // Child-first, and both passes complete before anything is released: `onExitTree` is specified
    // child-first, and `onDestroy` "before components are released".
    for (usize index = subtree.size(); index > 0; --index) {
        (void)behaviours_.invoke(*this, Node(*this, subtree[index - 1]),
                                 BehaviourCallback::ExitTree, 0.0F, nullptr);
    }
    for (usize index = subtree.size(); index > 0; --index) {
        const Node member(*this, subtree[index - 1]);
        if (Status detached = behaviours_.detach(*this, member); !detached) {
            return detached;
        }
        if (const Name alias = member.alias(); !alias.is_empty()) {
            if (Entity* held = aliases_.find(alias.index()); held != nullptr) {
                *held = ecs::kNoEntity;
            }
        }
    }

    const Entity parent = world_->parent_of(node.entity());
    if (Status destroyed = world_->destroy(node.entity(), ecs::DestroyPolicy::CascadeChildren);
        !destroyed) {
        return destroyed;
    }
    return renumber_children(parent);
}

Status SceneTree::renumber_children(Entity parent) noexcept {
    if (!parent.valid() || !world_->is_alive(parent)) {
        return ok();
    }
    scratch_children_.clear();
    for (const Entity child : world_->children_of(parent)) {
        if (Status pushed = scratch_children_.push_back(child); !pushed) {
            return pushed;
        }
    }
    const World& world = *world_;
    const ComponentTypeId order_id = ids_.child_order;
    std::ranges::sort(scratch_children_, [&world, order_id](Entity left, Entity right) noexcept {
        const auto* left_order = world.get<ChildOrder>(left, order_id);
        const auto* right_order = world.get<ChildOrder>(right, order_id);
        const u32 left_value = (left_order == nullptr) ? 0U : left_order->value;
        const u32 right_value = (right_order == nullptr) ? 0U : right_order->value;
        // The entity index breaks the tie, so the result does not depend on the order the
        // `Children` buffer happens to be in after a swap-removal.
        return (left_value != right_value) ? (left_value < right_value)
                                           : (left.index() < right.index());
    });
    for (usize index = 0; index < scratch_children_.size(); ++index) {
        if (auto* order = world_->get_mut<ChildOrder>(scratch_children_[index], order_id);
            order != nullptr) {
            order->value = static_cast<u32>(index);
        }
    }
    return ok();
}

// --- Addressing -------------------------------------------------------------------------------

Node SceneTree::find(std::string_view path) noexcept {
    if (path.empty() || path.front() != '/') {
        return {};
    }
    path.remove_prefix(1);
    if (path.empty()) {
        return root();
    }
    return root().find(path);
}

Node SceneTree::find_alias(Name alias) noexcept {
    const Entity* held = aliases_.find(alias.index());
    if (held == nullptr) {
        return {};
    }
    Node node(*this, *held);
    // Verified against the entity's own `NodeAlias` before it is returned: the map is an index over
    // the components and never a second truth, so an entry that has gone stale resolves to nothing
    // rather than to the wrong node.
    return (node.valid() && node.alias() == alias) ? node : Node();
}

Status SceneTree::set_alias(Node node, Name alias) noexcept {
    if (!node.valid()) {
        return fail(ErrorCode::NotFound, "set_alias() on a node that is not valid");
    }
    if (alias.is_empty()) {
        return clear_alias(node);
    }
    if (const Node holder = find_alias(alias); holder.valid() && holder != node) {
        return fail(ErrorCode::AlreadyExists, "another node already holds this alias");
    }
    if (Status cleared = clear_alias(node); !cleared) {
        return cleared;
    }
    const NodeAlias value{alias};
    if (!world_->has(node.entity(), ids_.node_alias)) {
        if (Status added = world_->add(node.entity(), ids_.node_alias, &value); !added) {
            return added;
        }
    } else {
        *world_->get_mut<NodeAlias>(node.entity(), ids_.node_alias) = value;
    }
    if (Entity* held = aliases_.find(alias.index()); held != nullptr) {
        *held = node.entity();
        return ok();
    }
    Expected<Entity*, Error> slot = aliases_.insert(alias.index(), node.entity());
    if (!slot) {
        return make_unexpected(slot.error());
    }
    return ok();
}

Status SceneTree::clear_alias(Node node) noexcept {
    if (!node.valid() || !world_->has(node.entity(), ids_.node_alias)) {
        return ok();
    }
    const Name held = node.alias();
    if (Entity* entry = aliases_.find(held.index()); entry != nullptr && *entry == node.entity()) {
        *entry = ecs::kNoEntity;
    }
    return world_->remove(node.entity(), ids_.node_alias);
}

u32 SceneTree::alias_count() const noexcept {
    u32 live = 0;
    for (const auto& entry : aliases_) {
        if (entry.value.valid() && world_->is_alive(entry.value)) {
            ++live;
        }
    }
    return live;
}

Status SceneTree::roots(Array<Node>& out) noexcept {
    ecs::QueryDesc desc(allocator());
    if (Status with = desc.with(ids_.node_name); !with) {
        return with;
    }
    if (Status without = desc.without(world_->parent_component()); !without) {
        return without;
    }
    ecs::Query query(*world_, std::move(desc));
    Array<Entity> found(allocator());
    Status collected = ok();
    Status iterated = query.for_each_chunk([&found, &collected](ecs::QueryChunk& chunk) noexcept {
        if (!collected) {
            return;
        }
        for (const Entity entity : chunk.entities()) {
            if (Status pushed = found.push_back(entity); !pushed) {
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
        found, [](Entity left, Entity right) noexcept { return left.index() < right.index(); });
    for (const Entity entity : found) {
        if (Status pushed = out.push_back(Node(*this, entity)); !pushed) {
            return pushed;
        }
    }
    return ok();
}

// --- The frame --------------------------------------------------------------------------------

Status SceneTree::queue_lifecycle(Entity subtree_root, bool attached) noexcept {
    return pending_.push_back(LifecycleEvent{subtree_root, attached});
}

Status SceneTree::dispatch_attached(Entity subtree_root) noexcept {
    Array<Entity> subtree(allocator());
    if (Status collected = collect_subtree(subtree_root, subtree); !collected) {
        return collected;
    }
    // Parent-first, as the specification tabulates `onEnterTree`.
    for (const Entity entity : subtree) {
        (void)behaviours_.invoke(*this, Node(*this, entity), BehaviourCallback::EnterTree, 0.0F,
                                 nullptr);
    }
    // Child-first, and once per attachment: "every descendant SHALL receive `onReady` before its
    // ancestors, exactly once".
    for (usize index = subtree.size(); index > 0; --index) {
        const Node node(*this, subtree[index - 1]);
        const u32 instance = behaviours_.instance_of(*this, node.entity());
        if (instance == kNoBehaviourInstance || behaviours_.ready(instance)) {
            continue;
        }
        behaviours_.set_ready(instance, true);
        (void)behaviours_.invoke(*this, node, BehaviourCallback::Ready, 0.0F, nullptr);
    }
    return ok();
}

Status SceneTree::dispatch_detached(Entity subtree_root) noexcept {
    Array<Entity> subtree(allocator());
    if (Status collected = collect_subtree(subtree_root, subtree); !collected) {
        return collected;
    }
    for (usize index = subtree.size(); index > 0; --index) {
        const Node node(*this, subtree[index - 1]);
        (void)behaviours_.invoke(*this, node, BehaviourCallback::ExitTree, 0.0F, nullptr);
        // Leaving the tree ends the attachment, so a later re-attachment gets `onReady` again —
        // which is the "once per attachment" half of the rule rather than "once ever".
        if (const u32 instance = behaviours_.instance_of(*this, node.entity());
            instance != kNoBehaviourInstance) {
            behaviours_.set_ready(instance, false);
        }
    }
    return ok();
}

Status SceneTree::pump() noexcept {
    // An instance whose entity a system destroyed through a command buffer is reclaimed first, so
    // nothing below dispatches into a node that is no longer there.
    (void)behaviours_.reclaim_dead(*this);

    dispatching_.clear();
    if (Status moved = dispatching_.append(pending_.span()); !moved) {
        return moved;
    }
    pending_.clear();
    for (const LifecycleEvent& event : dispatching_) {
        if (!world_->is_alive(event.subtree_root)) {
            continue;
        }
        const Status dispatched = event.attached ? dispatch_attached(event.subtree_root)
                                                 : dispatch_detached(event.subtree_root);
        if (!dispatched) {
            return dispatched;
        }
    }
    return behaviours_.sync_enablement(*this);
}

Status SceneTree::propagate(PropagationPhase phase, PropagationStats* out) noexcept {
    return scene::propagate(*this, phase, nullptr, out);
}

Status SceneTree::install_systems(ecs::Schedule& schedule) noexcept {
    if (Status installed = install_propagation_systems(*this, schedule); !installed) {
        return installed;
    }
    return behaviours_.install(*this, schedule);
}

SceneTreeStats SceneTree::stats() const noexcept {
    SceneTreeStats stats;
    stats.aliases = alias_count();
    stats.groups = groups_.size();
    stats.behaviour_types = behaviours_.size();
    stats.behaviour_instances = behaviours_.instance_count();
    // Counted from the ECS rather than from a running total the tree keeps: a total would be a
    // second representation of a fact the world already holds, and the first entity destroyed
    // outside the node API would make the two disagree.
    ecs::QueryDesc desc(allocator());
    if (desc.with(ids_.node_name) && desc.read(ids_.child_order)) {
        ecs::Query query(*world_, std::move(desc));
        u32 nodes = 0;
        u32 roots = 0;
        const ComponentTypeId parent_id = world_->parent_component();
        (void)query.for_each_chunk([&nodes, &roots, parent_id](ecs::QueryChunk& chunk) noexcept {
            nodes += chunk.count();
            if (!chunk.has(parent_id)) {
                roots += chunk.count();
            }
        });
        stats.nodes = nodes;
        stats.roots = roots;
    }
    stats.pending_lifecycle_events = pending_.size();
    for (const SceneRecord* record : scenes_) {
        if (record->status == SceneStatus::Unloaded) {
            continue;
        }
        ++stats.scenes_loaded;
        if (record->status == SceneStatus::Active) {
            ++stats.scenes_active;
        }
    }
    return stats;
}

}  // namespace cy::scene
