// The systems, the schema, and the reproduction check. See simulation.h for the reasoning.

#include "simulation.h"

#include <cy/core/determinism/state_schema.h>
#include <cy/ecs/snapshot.h>
#include <cy/runtime/state_hash.h>
#include <cy/scene/components.h>
#include <cy/scene/propagation.h>

#include <cstdio>
#include <utility>

namespace sample {
namespace {

using cy::Error;
using cy::Expected;
using cy::Span;
using cy::Status;
using cy::determinism::SimulationClass;
using cy::determinism::StateField;
using cy::ecs::Entity;

/// The ten floats of a `cy::Transform`, at their offsets, as the schema sees them.
///
/// `id` is chosen by the declarer here rather than taken from a `FieldId`, because the reflected
/// descriptor has one field where the schema needs ten. The rule the schema states is that an id is
/// stable across runs and unique within the subject; `base + n` satisfies both, and `base` differs
/// per subject only so that a divergence report reads unambiguously.
template <cy::usize kCount>
void describe_transform(StateField (&fields)[kCount], cy::u32 base_offset, cy::u64 base_id,
                        SimulationClass classification) noexcept {
    static_assert(kCount == 10, "a cy::Transform is a quaternion and two vectors");
    static const char* const kNames[] = {
        "rotation.x",    "rotation.y",    "rotation.z", "rotation.w", "translation.x",
        "translation.y", "translation.z", "scale.x",    "scale.y",    "scale.z"};
    for (cy::u32 index = 0; index < kCount; ++index) {
        fields[index].name = kNames[index];
        fields[index].id = base_id + index;
        fields[index].offset = base_offset + (index * static_cast<cy::u32>(sizeof(f32)));
        fields[index].kind = cy::reflect::FieldKind::F32;
        fields[index].classification = classification;
    }
}

// --- The system bodies
// ----------------------------------------------------------------------------

void drift_body(const cy::ecs::SystemContext& context) noexcept;
void sweep_body(const cy::ecs::SystemContext& context) noexcept;

}  // namespace

// --- The schema
// -----------------------------------------------------------------------------------

Status declare_state_schema(cy::runtime::Simulation& simulation, const Components& ids) noexcept {
    cy::determinism::StateSchema& schema = simulation.schema();

    // `Placement`: authored, and the position every cooked entity settles at.
    StateField placement[10];
    describe_transform(placement, static_cast<cy::u32>(offsetof(Placement, local)), 1,
                       SimulationClass::Authoritative);
    if (Status declared =
            schema.declare(cy::runtime::subject_of(ids.placement), "sample::Placement",
                           Span<const StateField>(placement, 10));
        !declared) {
        return declared;
    }

    // `scene::LocalTransform`: the authored half of the node hierarchy. `WorldTransform` is
    // deliberately NOT declared — it is derived from this and its ancestors, and `Derived` state is
    // never hashed. Declaring both would fold the same fact in twice and would report a divergence
    // in two places for one cause.
    cy::scene::SceneTree* tree = simulation.tree();
    if (tree == nullptr) {
        return cy::ok();
    }
    StateField local[10];
    describe_transform(local, static_cast<cy::u32>(offsetof(cy::scene::LocalTransform, value)), 101,
                       SimulationClass::Authoritative);
    return schema.declare(cy::runtime::subject_of(tree->components().local_transform),
                          "cy::scene::LocalTransform", Span<const StateField>(local, 10));
}

// --- The systems
// ----------------------------------------------------------------------------------

Systems::Systems(cy::runtime::Simulation& simulation, const Components& ids,
                 const NodeReport& nodes) noexcept
    : simulation_(&simulation),
      ids_(ids),
      drift_query_(simulation.world(), cy::ecs::QueryDesc(simulation.world().allocator())) {
    drift_.simulation = &simulation;
    drift_.drift = ids.drift;
    sweep_.simulation = &simulation;
    sweep_.tree = simulation.tree();
    sweep_.batteries = nodes.batteries_entities.span();
}

Status Systems::install() noexcept {
    // THE QUERY IS THE DECLARATION. `write()` records into the query's own `AccessSet` as the term
    // is added, so the parallelism the scheduler derives cannot drift away from what the body
    // touches — see `<cy/ecs/system.h>`.
    cy::ecs::QueryDesc desc(simulation_->world().allocator());
    if (Status declared = desc.write(ids_.drift); !declared) {
        return declared;
    }
    const cy::jobs::AccessSet drift_access = desc.access();
    drift_query_ = cy::ecs::Query(simulation_->world(), std::move(desc));
    drift_.query = &drift_query_;
    drift_.stream = simulation_->random().stream("sample.drift");

    cy::ecs::SystemDesc drift;
    drift.name = "sample.drift";
    drift.body = &drift_body;
    drift.user = &drift_;
    drift.access = drift_access;
    if (const Expected<cy::ecs::SystemId, Error> added =
            simulation_->schedule().add(cy::ecs::Stage::Simulation, drift);
        !added) {
        return cy::make_unexpected(added.error());
    }
    ++systems_;

    if (sweep_.tree == nullptr || sweep_.batteries.empty()) {
        return cy::ok();
    }

    // `sweep` has no query: it moves three nodes the scene load already identified, and a query
    // that walked every node every tick to find them would be doing the load's work again. So its
    // access set is written out — the one place in this sample where the declaration and the body
    // are two objects, and the reason is stated rather than assumed.
    const cy::scene::SceneComponents& scene_ids = sweep_.tree->components();
    sweep_.local_transform = scene_ids.local_transform;
    cy::jobs::AccessSet sweep_access;
    if (Status declared = sweep_access.write(scene_ids.local_transform); !declared) {
        return declared;
    }
    // `mark_transform_changed` writes the node's `NodeState` and its ancestors'.
    if (Status declared = sweep_access.write(scene_ids.state); !declared) {
        return declared;
    }
    if (Status declared = sweep_access.read(simulation_->world().parent_component()); !declared) {
        return declared;
    }

    cy::ecs::SystemDesc sweep;
    sweep.name = "sample.sweep";
    sweep.body = &sweep_body;
    sweep.user = &sweep_;
    sweep.access = sweep_access;
    if (const Expected<cy::ecs::SystemId, Error> added =
            simulation_->schedule().add(cy::ecs::Stage::Simulation, sweep);
        !added) {
        return cy::make_unexpected(added.error());
    }
    ++systems_;
    return cy::ok();
}

namespace {

/// Integrate every cooked entity, with a jitter drawn from a named seeded stream.
///
/// Three lines carry the whole determinism argument: the point comes from the clock, the step comes
/// from the clock, and the jitter is a pure function of (stream, point, entity, index). Nothing
/// here keeps a counter, so two entities in different chunks — or the same entity in a run that
/// packed the chunks differently — get the same value.
void drift_body(const cy::ecs::SystemContext& context) noexcept {
    auto* state = static_cast<Systems::DriftState*>(context.user);
    const cy::determinism::SimulationPoint now = state->simulation->clock().now();
    const f32 step = state->simulation->clock().delta_seconds();

    (void)state->query->for_each_chunk([&](cy::ecs::QueryChunk& chunk) noexcept {
        const Span<const Entity> keys = chunk.entities();
        const Span<Drift> values = chunk.write<Drift>(state->drift);
        for (cy::usize row = 0; row < values.size(); ++row) {
            const f32 jitter = state->stream.unit_float(now, keys[row].index(), 0) - 0.5F;
            values[row].x += step * (1.0F + jitter);
            values[row].y += step * jitter;
            values[row].last_tick = static_cast<u32>(now.tick);
        }
    });
}

/// Swing the battery nodes, so that the tree has something to propagate.
///
/// Written through the ECS rather than through `Node::set_local_transform` on purpose: this is a
/// system, and a system writes columns. `mark_transform_changed` is the spelling propagation.h
/// names for exactly that — it is what lets the walk skip the subtrees that did not move, and a
/// system that wrote the column without it would leave the world transforms a tick behind.
void sweep_body(const cy::ecs::SystemContext& context) noexcept {
    auto* state = static_cast<Systems::SweepState*>(context.user);
    const cy::determinism::SimulationPoint now = state->simulation->clock().now();
    const f32 seconds = static_cast<f32>(state->simulation->clock().seconds());

    for (cy::usize index = 0; index < state->batteries.size(); ++index) {
        const Entity entity = state->batteries[index];
        auto* local =
            context.world->get_mut<cy::scene::LocalTransform>(entity, state->local_transform);
        if (local == nullptr) {
            continue;
        }
        // A function of the clock and the node's ordinal. No wall clock, no accumulated state: the
        // value at tick N is the same whichever ticks came before it.
        const f32 phase = seconds + static_cast<f32>(index);
        local->value.translation.x = (static_cast<f32>(index) * 10.0F) + phase;
        local->value.translation.z = static_cast<f32>(now.tick % 64U) * 0.125F;
        (void)cy::scene::mark_transform_changed(*state->tree, entity);
    }
}

}  // namespace

// --- The hash, and the reproduction check
// ----------------------------------------------------------

Expected<cy::u64, Error> hash_world_now(cy::runtime::Simulation& simulation,
                                        cy::determinism::StateHashTree& tree,
                                        cy::runtime::WorldHashReport& report) noexcept {
    if (Status hashed =
            cy::runtime::hash_world(simulation.world(), simulation.schema(), tree, report);
        !hashed) {
        return cy::make_unexpected(hashed.error());
    }
    return report.hash;
}

Status check_snapshot_restore(cy::runtime::Simulation& simulation, cy::u32 diverging_ticks,
                              ReproductionReport& report) noexcept {
    cy::Allocator& allocator = simulation.world().allocator();
    cy::determinism::StateHashTree settled(allocator);
    cy::determinism::StateHashTree restored(allocator);
    cy::runtime::WorldHashReport walk;

    const Expected<cy::u64, Error> before = hash_world_now(simulation, settled, walk);
    if (!before) {
        return cy::make_unexpected(before.error());
    }
    report.settled = before.value();

    cy::ecs::Snapshot snapshot(allocator);
    if (Status captured = snapshot.capture(simulation.world()); !captured) {
        return captured;
    }
    report.snapshot_bytes = snapshot.bytes();
    report.snapshot_entities = snapshot.entity_count();

    // Move the world on, so that the restore has something to undo. A check whose "before" and
    // "after" were never allowed to differ would pass on a restore that did nothing at all.
    report.diverging_ticks = diverging_ticks;
    for (cy::u32 tick = 0; tick < diverging_ticks; ++tick) {
        if (const Expected<cy::determinism::CommitRecord, Error> stepped = simulation.step(nullptr);
            !stepped) {
            return cy::make_unexpected(stepped.error());
        }
    }
    cy::determinism::StateHashTree moved(allocator);
    const Expected<cy::u64, Error> after = hash_world_now(simulation, moved, walk);
    if (!after) {
        return cy::make_unexpected(after.error());
    }
    report.diverged = after.value();
    report.moved = report.diverged != report.settled;

    if (Status put_back = snapshot.restore(simulation.world()); !put_back) {
        return put_back;
    }
    const Expected<cy::u64, Error> again = hash_world_now(simulation, restored, walk);
    if (!again) {
        return cy::make_unexpected(again.error());
    }
    report.restored = again.value();
    report.matches = report.restored == report.settled;

    if (!report.matches) {
        // The hierarchy earning its keep: the comparison names the subtree that disagrees rather
        // than reporting two different numbers and leaving a person to find out where.
        cy::determinism::Divergence divergence;
        cy::determinism::StateHashTree::compare(settled, restored, divergence);
        usize used = 0;
        for (cy::u32 level = 0; level < divergence.depth; ++level) {
            used += static_cast<usize>(std::snprintf(
                report.divergence + used, sizeof(report.divergence) - used, "%s%s:%llu",
                level == 0 ? "" : "/", cy::determinism::hash_level_name(divergence.levels[level]),
                static_cast<unsigned long long>(divergence.ids[level])));
            if (used + 1 >= sizeof(report.divergence)) {
                break;
            }
        }
    }
    return cy::ok();
}

void print_hash_tree(const cy::determinism::StateHashTree& tree, const char* tag) noexcept {
    const Span<const cy::determinism::HashNode> nodes = tree.nodes();
    for (cy::u32 index = 0; index < nodes.size(); ++index) {
        // Depth by walking parents. The top two levels only: the point of a hierarchy is that a
        // divergence narrows to a subtree, and printing every entity would bury that.
        cy::u32 depth = 0;
        for (cy::u32 parent = nodes[index].parent;
             parent != cy::determinism::HashNode::kNoNode && depth < 4;
             parent = nodes[parent].parent) {
            ++depth;
        }
        if (depth > 1) {
            continue;
        }
        std::fprintf(stdout, "%s: level    %*s%-9s %-24s %016llx  children=%u\n", tag,
                     static_cast<int>(depth * 2), "",
                     cy::determinism::hash_level_name(nodes[index].level), nodes[index].name,
                     static_cast<unsigned long long>(nodes[index].hash), nodes[index].child_count);
    }
}

}  // namespace sample
