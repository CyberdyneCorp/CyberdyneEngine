// Transform, visibility and enablement propagation. Tasks 3.1.3 and 3.1.5.
//
// THE ORDER OF OPERATIONS INSIDE `visit_node` IS LOAD-BEARING AND IS NOT AN ACCIDENT. Adding or
// removing a tag is a structural change: the entity moves to a different archetype, which moves its
// row, which invalidates every pointer into its old chunk. So the walk reads and writes every
// component through pointers first, copies out what it still needs, collects the children, and only
// then applies the tag changes. Doing it in the readable order — flags, then tags, then transforms
// — would write a `WorldTransform` through a pointer into a chunk the entity had already left.

#include <cy/scene/propagation.h>

#include <cy/core/memory/array.h>
#include <cy/ecs/query.h>
#include <cy/scene/tree.h>

#include <algorithm>

namespace cy::scene {
namespace {

/// One node still to visit, and what its parent decided.
struct Frame {
    Entity entity;
    Transform parent_world;
    bool has_parent = false;
    /// The parent's transform was recomputed, so this node's derived one is stale whatever its own
    /// dirty bit says.
    bool transform_dirty = false;
    bool flags_dirty = false;
    bool parent_visible = true;
    bool parent_enabled = true;
};

/// Everything the walk needs, gathered once instead of being threaded through six parameters.
struct Walk {
    SceneTree* tree = nullptr;
    World* world = nullptr;
    const SceneComponents* ids = nullptr;
    PropagationPhase phase = PropagationPhase::Simulation;
    ecs::CommandBuffer* commands = nullptr;
    PropagationStats* stats = nullptr;
};

/// What `visit_node` decided about one node, before anything structural is applied.
struct NodeOutcome {
    Transform world_transform;
    bool visible = true;
    bool enabled = true;
    bool descend_transform = false;
    bool descend_flags = false;
    bool recomputed_transform = false;
    bool recomputed_flags = false;
};

/// Add or remove a tag so that its presence matches `wanted`. Recorded rather than applied when the
/// walk is running inside a stage.
[[nodiscard]] Status set_tag(const Walk& walk, Entity entity, ComponentTypeId tag,
                             bool wanted) noexcept {
    const bool present = walk.world->has(entity, tag);
    if (present == wanted) {
        return ok();
    }
    if (wanted) {
        ++walk.stats->tags_added;
        return (walk.commands != nullptr) ? walk.commands->add(entity, tag)
                                          : walk.world->add(entity, tag);
    }
    ++walk.stats->tags_removed;
    return (walk.commands != nullptr) ? walk.commands->remove(entity, tag)
                                      : walk.world->remove(entity, tag);
}

/// Recompute one node's `WorldTransform`, rolling the interpolation history when this is the
/// simulation-phase pass.
void recompute_transform(const Walk& walk, Entity entity, const Frame& frame,
                         NodeOutcome& outcome) noexcept {
    const auto* local = walk.world->get<LocalTransform>(entity, walk.ids->local_transform);
    auto* derived = walk.world->get_mut<WorldTransform>(entity, walk.ids->world_transform);
    if (local == nullptr || derived == nullptr) {
        return;
    }
    // A root's world transform IS its local one, assigned rather than multiplied by an identity, so
    // `scene-graph-and-nodes`' "a root node's `LocalTransform` SHALL equal its `WorldTransform`" is
    // exact and not merely close.
    const Transform previous = derived->value;
    derived->value = frame.has_parent ? (frame.parent_world * local->value) : local->value;
    outcome.world_transform = derived->value;
    outcome.recomputed_transform = true;

    if (walk.phase != PropagationPhase::Simulation) {
        return;
    }
    auto* history =
        walk.world->get_mut<InterpolatedTransform>(entity, walk.ids->interpolated_transform);
    if (history == nullptr) {
        return;
    }
    // A teleported node's history collapses onto the new placement, so any blend a renderer does is
    // a blend between two equal values — which is "a teleport flag SHALL suppress interpolation for
    // that frame" expressed as data rather than as a branch the renderer has to remember.
    history->previous = history->teleport ? derived->value : previous;
    history->teleport = false;
}

/// Read and update the node's dirty bits, recompute what is stale, and report what the children
/// need. Nothing structural happens here; see the file header.
[[nodiscard]] NodeOutcome examine(const Walk& walk, const Frame& frame, bool& is_node) noexcept {
    NodeOutcome outcome;
    auto* state = walk.world->get_mut<NodeState>(frame.entity, walk.ids->state);
    is_node = state != nullptr;
    if (!is_node) {
        return outcome;
    }

    const u8 dirty = state->dirty;
    const bool recompute = frame.transform_dirty || (dirty & kDirtyTransform) != 0;
    if (recompute) {
        recompute_transform(walk, frame.entity, frame, outcome);
    } else {
        const auto* derived =
            walk.world->get<WorldTransform>(frame.entity, walk.ids->world_transform);
        outcome.world_transform = (derived == nullptr) ? Transform::identity() : derived->value;
    }

    const auto* flags = walk.world->get<NodeFlags>(frame.entity, walk.ids->flags);
    outcome.visible = frame.parent_visible && (flags == nullptr || flags->visible);
    outcome.enabled = frame.parent_enabled && (flags == nullptr || flags->enabled);
    outcome.recomputed_flags = frame.flags_dirty || (dirty & kDirtyFlags) != 0;

    outcome.descend_transform = recompute || (dirty & kDirtySubtreeTransform) != 0;
    outcome.descend_flags = outcome.recomputed_flags || (dirty & kDirtySubtreeFlags) != 0;

    // `state` is still valid: nothing structural has happened yet.
    state->dirty = static_cast<u8>(
        dirty & ~(kDirtyTransform | kDirtySubtreeTransform | kDirtyFlags | kDirtySubtreeFlags));
    return outcome;
}

/// The node's children, in authored order, into `out` (cleared first).
[[nodiscard]] Status ordered_children(const Walk& walk, Entity entity,
                                      Array<Entity>& out) noexcept {
    out.clear();
    for (const Entity child : walk.world->children_of(entity)) {
        if (Status pushed = out.push_back(child); !pushed) {
            return pushed;
        }
    }
    const World& world = *walk.world;
    const ComponentTypeId order_id = walk.ids->child_order;
    std::ranges::sort(out, [&world, order_id](Entity left, Entity right) noexcept {
        const auto* left_order = world.get<ChildOrder>(left, order_id);
        const auto* right_order = world.get<ChildOrder>(right, order_id);
        const u32 left_value = (left_order == nullptr) ? 0U : left_order->value;
        const u32 right_value = (right_order == nullptr) ? 0U : right_order->value;
        return (left_value != right_value) ? (left_value < right_value)
                                           : (left.index() < right.index());
    });
    return ok();
}

/// Publish the node's effective state as the `Hidden` and `Disabled` tags. Structural, so it runs
/// after every pointer into the node's chunk has been finished with — see the file header.
[[nodiscard]] Status publish_flags(const Walk& walk, Entity entity,
                                   const NodeOutcome& outcome) noexcept {
    if (!outcome.recomputed_flags) {
        return ok();
    }
    if (Status hidden = set_tag(walk, entity, walk.ids->hidden, !outcome.visible); !hidden) {
        return hidden;
    }
    return set_tag(walk, entity, walk.ids->disabled, !outcome.enabled);
}

void account(const Walk& walk, const NodeOutcome& outcome) noexcept {
    ++walk.stats->nodes_visited;
    walk.stats->transforms_recomputed += outcome.recomputed_transform ? 1U : 0U;
    walk.stats->flags_recomputed += outcome.recomputed_flags ? 1U : 0U;
    walk.stats->subtrees_skipped +=
        (!outcome.descend_transform && !outcome.descend_flags) ? 1U : 0U;
}

/// Push a node's children onto the walk's stack, in reverse so they pop in authored order — which
/// is what makes the traversal order a property of the data rather than of the container.
[[nodiscard]] Status push_children(const NodeOutcome& outcome, Span<const Entity> children,
                                   Array<Frame>& stack) noexcept {
    for (usize index = children.size(); index > 0; --index) {
        Frame child;
        child.entity = children[index - 1];
        child.parent_world = outcome.world_transform;
        child.has_parent = true;
        child.transform_dirty = outcome.recomputed_transform;
        child.flags_dirty = outcome.recomputed_flags;
        child.parent_visible = outcome.visible;
        child.parent_enabled = outcome.enabled;
        if (Status pushed = stack.push_back(child); !pushed) {
            return pushed;
        }
    }
    return ok();
}

[[nodiscard]] Status visit_node(const Walk& walk, const Frame& frame, Array<Frame>& stack,
                                Array<Entity>& children) noexcept {
    bool is_node = false;
    const NodeOutcome outcome = examine(walk, frame, is_node);
    if (!is_node) {
        return ok();
    }
    account(walk, outcome);

    const bool descend = outcome.descend_transform || outcome.descend_flags;
    if (descend) {
        if (Status collected = ordered_children(walk, frame.entity, children); !collected) {
            return collected;
        }
    }
    // Structural last: everything above holds pointers into this entity's chunk, and a tag change
    // moves the row out of it.
    if (Status published = publish_flags(walk, frame.entity, outcome); !published) {
        return published;
    }
    return descend ? push_children(outcome, children.span(), stack) : ok();
}

/// Every node with no parent, sorted by entity index so the traversal does not depend on the order
/// entities happened to be created in.
[[nodiscard]] Status collect_roots(SceneTree& tree, Array<Entity>& out) noexcept {
    World& world = tree.world();
    ecs::QueryDesc desc(tree.allocator());
    if (Status with = desc.with(tree.components().node_name); !with) {
        return with;
    }
    if (Status without = desc.without(world.parent_component()); !without) {
        return without;
    }
    ecs::Query query(world, std::move(desc));
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

/// Mark one node dirty and tell its ancestors that something below them is.
[[nodiscard]] Status mark(SceneTree& tree, Entity entity, u8 own_bit, u8 subtree_bit) noexcept {
    World& world = tree.world();
    const ComponentTypeId state_id = tree.components().state;
    auto* state = world.get_mut<NodeState>(entity, state_id);
    if (state == nullptr) {
        return fail(ErrorCode::NotFound, "this entity has no NodeState: it is not a node");
    }
    state->dirty = static_cast<u8>(state->dirty | own_bit | subtree_bit);
    // Stopping at the first ancestor that already knows is what keeps a burst of writes in one
    // subtree from costing depth each: the second write walks one edge.
    for (Entity ancestor = world.parent_of(entity); ancestor.valid();
         ancestor = world.parent_of(ancestor)) {
        auto* above = world.get_mut<NodeState>(ancestor, state_id);
        if (above == nullptr || (above->dirty & subtree_bit) != 0) {
            break;
        }
        above->dirty = static_cast<u8>(above->dirty | subtree_bit);
    }
    return ok();
}

void run_propagation_system(const ecs::SystemContext& context, PropagationPhase phase) noexcept {
    auto* tree = static_cast<SceneTree*>(context.user);
    if (tree == nullptr) {
        return;
    }
    // The result is dropped deliberately: a system body cannot report, and the only failure
    // propagation has is an allocation for its own stack. The counters in `PropagationStats` are
    // what a diagnostic reads; a test calls `propagate()` directly and checks the status.
    (void)propagate(*tree, phase, context.commands, nullptr);
}

void simulation_propagation_body(const ecs::SystemContext& context) noexcept {
    run_propagation_system(context, PropagationPhase::Simulation);
}

void render_propagation_body(const ecs::SystemContext& context) noexcept {
    run_propagation_system(context, PropagationPhase::Render);
}

/// What both propagation systems declare: the derived state is written, the authored state is read.
[[nodiscard]] Expected<jobs::AccessSet, Error> propagation_access(const SceneTree& tree) noexcept {
    const SceneComponents& ids = tree.components();
    jobs::AccessSet access;
    // The relation is read as well as the components: the walk asks for a node's parent and its
    // children, and a declaration that omitted them would let the scheduler run this beside a
    // system reparenting entities.
    const ComponentTypeId reads[] = {ids.local_transform,
                                     ids.flags,
                                     ids.child_order,
                                     ids.node_name,
                                     tree.world().parent_component(),
                                     tree.world().children_component()};
    const ComponentTypeId writes[] = {ids.world_transform, ids.state, ids.interpolated_transform,
                                      ids.hidden, ids.disabled};
    for (const ComponentTypeId component : reads) {
        if (Status declared = access.read(component); !declared) {
            return make_unexpected(declared.error());
        }
    }
    for (const ComponentTypeId component : writes) {
        if (Status declared = access.write(component); !declared) {
            return make_unexpected(declared.error());
        }
    }
    return access;
}

}  // namespace

Status mark_transform_changed(SceneTree& tree, Entity entity) noexcept {
    return mark(tree, entity, kDirtyTransform, kDirtySubtreeTransform);
}

Status mark_flags_changed(SceneTree& tree, Entity entity) noexcept {
    return mark(tree, entity, kDirtyFlags, kDirtySubtreeFlags);
}

Status propagate(SceneTree& tree, PropagationPhase phase, ecs::CommandBuffer* commands,
                 PropagationStats* out) noexcept {
    PropagationStats local;
    Walk walk;
    walk.tree = &tree;
    walk.world = &tree.world();
    walk.ids = &tree.components();
    walk.phase = phase;
    walk.commands = commands;
    walk.stats = (out == nullptr) ? &local : out;
    *walk.stats = PropagationStats{};

    Array<Entity> roots(tree.allocator());
    if (Status collected = collect_roots(tree, roots); !collected) {
        return collected;
    }
    walk.stats->roots = static_cast<u32>(roots.size());

    // An explicit stack rather than recursion: an authored hierarchy has no depth a scene file
    // cannot exceed, and a stack overflow inside a propagation system is not diagnosable.
    Array<Frame> stack(tree.allocator());
    Array<Entity> children(tree.allocator());
    for (usize index = roots.size(); index > 0; --index) {
        Frame frame;
        frame.entity = roots[index - 1];
        if (Status pushed = stack.push_back(frame); !pushed) {
            return pushed;
        }
    }
    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();
        if (Status visited = visit_node(walk, frame, stack, children); !visited) {
            return visited;
        }
    }
    return ok();
}

Status install_propagation_systems(SceneTree& tree, ecs::Schedule& schedule) noexcept {
    Expected<jobs::AccessSet, Error> access = propagation_access(tree);
    if (!access) {
        return make_unexpected(access.error());
    }

    ecs::SystemDesc simulation;
    simulation.name = kSimulationPropagationSystem;
    simulation.body = &simulation_propagation_body;
    simulation.user = &tree;
    simulation.access = *access;
    if (Expected<ecs::SystemId, Error> added = schedule.add(ecs::Stage::PostSimulation, simulation);
        !added) {
        return make_unexpected(added.error());
    }

    // `scene-graph-and-nodes`: propagation runs "in `PostSimulation` and again before rendering".
    // The second one is in `Frame`, which is the first variable-step stage, so a node moved by
    // frame-rate logic is propagated before anything draws it.
    ecs::SystemDesc render;
    render.name = kRenderPropagationSystem;
    render.body = &render_propagation_body;
    render.user = &tree;
    render.access = *access;
    if (Expected<ecs::SystemId, Error> added = schedule.add(ecs::Stage::Frame, render); !added) {
        return make_unexpected(added.error());
    }
    return ok();
}

}  // namespace cy::scene
