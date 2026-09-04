// The five coherence invariants, checked. Task 3.1.10, design.md §3.

#include <cy/scene/coherence.h>

#include <cy/ecs/query.h>
#include <cy/scene/propagation.h>

#include <cmath>

namespace cy::scene {
namespace {

void record(CoherenceReport& report, Invariant invariant, Entity entity,
            const char* detail) noexcept {
    ++report.violations;
    if (report.recorded_count < CoherenceReport::kMaxRecorded) {
        report.recorded[report.recorded_count++] = CoherenceViolation{invariant, entity, detail};
    }
}

[[nodiscard]] bool near_enough(f32 left, f32 right) noexcept {
    return std::fabs(left - right) <= kCoherenceTolerance;
}

[[nodiscard]] bool near_enough(Vec3 left, Vec3 right) noexcept {
    return near_enough(left.x, right.x) && near_enough(left.y, right.y) &&
           near_enough(left.z, right.z);
}

/// Two transforms are the same placement. Rotations are compared up to sign, because `q` and `-q`
/// are the same rotation and a recomputation may land on either.
[[nodiscard]] bool same_placement(const Transform& left, const Transform& right) noexcept {
    if (!near_enough(left.translation, right.translation) ||
        !near_enough(left.scale, right.scale)) {
        return false;
    }
    const f32 dot = (left.rotation.x * right.rotation.x) + (left.rotation.y * right.rotation.y) +
                    (left.rotation.z * right.rotation.z) + (left.rotation.w * right.rotation.w);
    return near_enough(std::fabs(dot), 1.0F);
}

/// Invariant 3: the ECS relation agrees with itself in both directions.
void check_relation(SceneTree& tree, Entity entity, CoherenceReport& report) noexcept {
    const World& world = tree.world();
    const Entity parent = world.parent_of(entity);
    if (parent.valid()) {
        if (!world.is_alive(parent)) {
            record(report, Invariant::ParentMatchesTree, entity, "the parent is not alive");
        } else {
            bool listed = false;
            for (const Entity child : world.children_of(parent)) {
                listed = listed || (child == entity);
            }
            if (!listed) {
                record(report, Invariant::ParentMatchesTree, entity,
                       "the parent's Children does not name this entity");
            }
        }
    }
    for (const Entity child : world.children_of(entity)) {
        if (!world.is_alive(child)) {
            record(report, Invariant::ParentMatchesTree, entity, "a listed child is not alive");
        } else if (world.parent_of(child) != entity) {
            record(report, Invariant::ParentMatchesTree, child,
                   "this entity's Parent is not the node listing it as a child");
        }
    }
}

/// Invariant 4, for a node whose transform propagation is up to date.
void check_transform(SceneTree& tree, Entity entity, CoherenceReport& report) noexcept {
    const World& world = tree.world();
    const SceneComponents& ids = tree.components();
    const auto* state = world.get<NodeState>(entity, ids.state);
    if (state == nullptr || (state->dirty & kDirtyTransform) != 0) {
        // Still marked: the node has moved and propagation has not run. That is the state
        // propagation exists to resolve, not an inconsistency.
        return;
    }
    const auto* local = world.get<LocalTransform>(entity, ids.local_transform);
    const auto* derived = world.get<WorldTransform>(entity, ids.world_transform);
    if (local == nullptr || derived == nullptr) {
        return;
    }
    const Entity parent = world.parent_of(entity);
    const auto* above =
        parent.valid() ? world.get<WorldTransform>(parent, ids.world_transform) : nullptr;
    const Transform expected = (above == nullptr) ? local->value : (above->value * local->value);
    if (!same_placement(expected, derived->value)) {
        record(report, Invariant::WorldTransformConsistent, entity,
               "WorldTransform is not LocalTransform composed with the parent chain");
    }
}

/// Invariant 5, for a node whose flag propagation is up to date.
void check_flags(SceneTree& tree, Entity entity, CoherenceReport& report) noexcept {
    const World& world = tree.world();
    const SceneComponents& ids = tree.components();
    const auto* state = world.get<NodeState>(entity, ids.state);
    if (state == nullptr || (state->dirty & kDirtyFlags) != 0) {
        return;
    }
    bool visible = true;
    bool enabled = true;
    for (Entity walk = entity; walk.valid(); walk = world.parent_of(walk)) {
        const auto* flags = world.get<NodeFlags>(walk, ids.flags);
        if (flags == nullptr) {
            continue;
        }
        visible = visible && flags->visible;
        enabled = enabled && flags->enabled;
    }
    if (world.has(entity, ids.hidden) == visible) {
        record(report, Invariant::EffectiveFlagsConsistent, entity,
               "the Hidden tag disagrees with the visibility of this node and its ancestors");
    }
    if (world.has(entity, ids.disabled) == enabled) {
        record(report, Invariant::EffectiveFlagsConsistent, entity,
               "the Disabled tag disagrees with the enablement of this node and its ancestors");
    }
}

void check_one(SceneTree& tree, Entity entity, CoherenceReport& report) noexcept {
    ++report.nodes_checked;
    const World& world = tree.world();
    if (!world.is_alive(entity)) {
        // Invariant 1. Unreachable through a query, which only yields live entities — checked
        // anyway, because the interesting failures arrive from outside the API and a check that
        // assumes its own correctness establishes nothing.
        record(report, Invariant::EntityAlive, entity, "the node's entity is not alive");
        return;
    }
    if (world.get(entity, tree.components().node_name) == nullptr) {
        // Invariant 2. A node *is* its entity: `NodeName` is the node, so "exactly one" is "the
        // component is present", and the ECS cannot hold two of one component on one entity.
        record(report, Invariant::OneNodePerEntity, entity, "the entity has no NodeName");
        return;
    }
    check_relation(tree, entity, report);
    check_transform(tree, entity, report);
    check_flags(tree, entity, report);
}

}  // namespace

const char* invariant_name(Invariant invariant) noexcept {
    switch (invariant) {
        case Invariant::EntityAlive:
            return "every node's entity is alive";
        case Invariant::OneNodePerEntity:
            return "every entity with a node has exactly one node";
        case Invariant::ParentMatchesTree:
            return "a node's ECS Parent matches its tree parent";
        case Invariant::WorldTransformConsistent:
            return "WorldTransform is consistent with LocalTransform and the parent chain";
        case Invariant::EffectiveFlagsConsistent:
            return "effective visibility and enablement are consistent with ancestors";
    }
    return "unknown";
}

Expected<CoherenceReport, Error> check_coherence(SceneTree& tree) noexcept {
    CoherenceReport report;
    Array<Entity> nodes(tree.allocator());

    ecs::QueryDesc desc(tree.allocator());
    if (Status with = desc.with(tree.components().node_name); !with) {
        return make_unexpected(with.error());
    }
    ecs::Query query(tree.world(), std::move(desc));
    Status collected = ok();
    Status iterated = query.for_each_chunk([&nodes, &collected](ecs::QueryChunk& chunk) noexcept {
        if (!collected) {
            return;
        }
        for (const Entity entity : chunk.entities()) {
            if (Status pushed = nodes.push_back(entity); !pushed) {
                collected = pushed;
                return;
            }
        }
    });
    if (!iterated) {
        return make_unexpected(iterated.error());
    }
    if (!collected) {
        return make_unexpected(collected.error());
    }

    // Checked outside the iteration: the walks below read `Children` buffers and parent chains, and
    // doing that inside a query would hold the world's iteration guard for the whole check.
    for (const Entity entity : nodes) {
        check_one(tree, entity, report);
    }
    return report;
}

bool coherent(SceneTree& tree) noexcept {
    const Expected<CoherenceReport, Error> report = check_coherence(tree);
    return report.has_value() && report->coherent();
}

}  // namespace cy::scene
