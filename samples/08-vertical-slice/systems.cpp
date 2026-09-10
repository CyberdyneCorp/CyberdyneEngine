// The game, run: four authored graphs compiled to four programs, the characters that execute them,
// and the tick that drives every system in one loop. M8.b section 12.
//
// READ THE TICK FIRST (`Slice::tick`, at the bottom). Everything above it exists to make those
// eight calls possible, and the eight calls are the milestone: think, sense, navigate, animate,
// act, present, assemble, fold.

#include "internals.h"

#include <cy/core/determinism/commit.h>
#include <cy/graph/audit.h>

#include "presentation.h"

#include <type_traits>

namespace cy::sample::slice {
namespace {

using cy::gameplay::abilities::AbilityDefinition;
using cy::gameplay::abilities::ApplyReport;
using cy::gameplay::abilities::AttributeDeclaration;
using cy::gameplay::abilities::Cost;
using cy::gameplay::abilities::CostKind;
using cy::gameplay::abilities::EffectDefId;
using cy::gameplay::abilities::EffectDefinition;
using cy::gameplay::abilities::EffectKind;
using cy::gameplay::abilities::EffectModifier;
using cy::gameplay::abilities::ModifierOp;
using cy::gameplay::abilities::StackingPolicy;

/// The level's positions, as the targeting rules ask for them. The host answers; the abilities
/// module has no world, which is why this function exists at all.
struct TargetWorld {
    static Span<const Vec3> positions;

    static bool position(Entity subject, Vec3& out, void* /*user*/) noexcept {
        const u32 index = subject.index();
        if (index == 0U || index > positions.size()) {
            return false;
        }
        out = positions[index - 1U];
        return true;
    }
    static bool visible(const Vec3&, const Vec3&, void*) noexcept { return true; }
};

Span<const Vec3> TargetWorld::positions;

/// A twelve-joint biped: a hip chain to a head, an arm, a leg. Two joints carry a bone level of
/// detail, because a level of detail that drops nothing is not one.
[[nodiscard]] Status build_biped(cy::animation::Skeleton& skeleton) noexcept {
    struct Row {
        const char* name = "";
        cy::u16 parent = 0;
        Vec3 translation;
        cy::u8 dropped_at = 0;
    };
    constexpr cy::u8 kKeep = cy::animation::kBoneLodLevels;
    const Row rows[kJointCount] = {
        {"root", cy::animation::kInvalidJoint, Vec3{0.0F, 0.0F, 0.0F}, kKeep},
        {"hips", 0, Vec3{0.0F, 0.9F, 0.0F}, kKeep},
        {"spine", 1, Vec3{0.0F, 0.3F, 0.0F}, kKeep},
        {"chest", 2, Vec3{0.0F, 0.3F, 0.0F}, kKeep},
        {"head", 3, Vec3{0.0F, 0.3F, 0.0F}, 2},
        {"shoulder", 3, Vec3{-0.2F, 0.2F, 0.0F}, kKeep},
        {"upper_arm", 5, Vec3{-0.25F, 0.0F, 0.0F}, kKeep},
        {"lower_arm", 6, Vec3{-0.25F, 0.0F, 0.0F}, kKeep},
        {"finger", 7, Vec3{-0.1F, 0.0F, 0.0F}, 1},
        {"upper_leg", 1, Vec3{-0.1F, -0.1F, 0.0F}, kKeep},
        {"lower_leg", 9, Vec3{0.0F, -0.4F, 0.0F}, kKeep},
        {"foot", 10, Vec3{0.0F, -0.4F, 0.0F}, kKeep},
    };
    for (const Row& row : rows) {
        Expected<cy::u16, Error> joint =
            skeleton.add_joint(Name::intern(row.name), row.parent,
                               Transform::from_translation(row.translation), row.dropped_at);
        if (!joint) {
            return Status{cy::make_unexpected(joint.error())};
        }
        if (Status bounds = skeleton.set_bounds(
                *joint,
                Aabb::from_center_extents(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.12F, 0.12F, 0.12F}));
            !bounds) {
            return bounds;
        }
    }
    return skeleton.finalize();
}

/// A one-second walk cycle that moves the root forward at `speed` metres a second and swings the
/// arm. Root motion is on, because that is what makes "root motion is integrated whatever the
/// tier" something this slice can measure.
[[nodiscard]] Status build_walk(cy::animation::Clip& clip, f32 speed) noexcept {
    clip.set_name(Name::intern("walk"));
    clip.set_duration(1.0F);
    clip.set_loop_mode(cy::animation::LoopMode::Loop);
    clip.set_sample_rate_hint(10.0F);
    clip.set_root_motion_joint(kJointRoot);

    Expected<u32, Error> root = clip.add_joint_track(
        cy::animation::TrackKind::Translation, kJointRoot, cy::animation::Interpolation::Linear);
    if (!root) {
        return Status{cy::make_unexpected(root.error())};
    }
    for (u32 frame = 0; frame <= 10U; ++frame) {
        const f32 time = static_cast<f32>(frame) / 10.0F;
        if (Status added = clip.add_key(*root, time, cy::Vec4{0.0F, 0.0F, -time * speed, 0.0F});
            !added) {
            return added;
        }
    }
    Expected<u32, Error> arm = clip.add_joint_track(cy::animation::TrackKind::Rotation, 6,
                                                    cy::animation::Interpolation::Spherical);
    if (!arm) {
        return Status{cy::make_unexpected(arm.error())};
    }
    for (u32 frame = 0; frame <= 10U; ++frame) {
        const f32 time = static_cast<f32>(frame) / 10.0F;
        const f32 swing = 0.4F * std::sin(time * 6.2831853F);
        if (Status added = clip.add_key(
                *arm, time, cy::Vec4{std::sin(swing * 0.5F), 0.0F, 0.0F, std::cos(swing * 0.5F)});
            !added) {
            return added;
        }
    }
    // COMPRESSION IS A COOK STEP AND SAMPLING READS THE COMPRESSED FORM ONLY. A clip that was
    // never compressed samples nothing and says so, which is the refusal this call answers —
    // `animation-and-skinning` puts the codec behind `compress()` on purpose, so a game that
    // authors a clip at run time still pays the cook's price before it can play it.
    const cy::animation::CompressionSettings settings;
    return clip.compress(settings);
}

/// An aim pose: the arm raised, no root motion. What the masked layer blends over the walk.
[[nodiscard]] Status build_aim(cy::animation::Clip& clip) noexcept {
    clip.set_name(Name::intern("aim"));
    clip.set_duration(0.5F);
    clip.set_loop_mode(cy::animation::LoopMode::Loop);
    clip.set_sample_rate_hint(10.0F);
    Expected<u32, Error> arm = clip.add_joint_track(cy::animation::TrackKind::Rotation, 6,
                                                    cy::animation::Interpolation::Spherical);
    if (!arm) {
        return Status{cy::make_unexpected(arm.error())};
    }
    for (u32 frame = 0; frame <= 5U; ++frame) {
        const f32 time = static_cast<f32>(frame) / 10.0F;
        if (Status added = clip.add_key(*arm, time, cy::Vec4{0.0F, 0.0F, 0.3827F, 0.9239F});
            !added) {
            return added;
        }
    }
    const cy::animation::CompressionSettings settings;
    return clip.compress(settings);
}

}  // namespace

// --- The four graphs, and the four programs they compile to
// ---------------------------------------

Status Slice::build_programs() noexcept {
    Brain& brain = *brain_;
    if (Status registered = cy::graph::behaviour::register_behaviour_nodes(brain.registry);
        !registered) {
        return registered;
    }
    if (Status registered = cy::graph::pose::register_pose_nodes(brain.registry); !registered) {
        return registered;
    }
    if (Status registered = cy::graph::script::register_script_nodes(brain.registry); !registered) {
        return registered;
    }
    if (Status registered = cy::graph::script::register_ability_nodes(brain.registry);
        !registered) {
        return registered;
    }
    if (Status registered = cy::graph::camera::register_camera_nodes(brain.registry); !registered) {
        return registered;
    }

    // --- The squad's behaviour: a selector over "engage what we can see" and "advance".
    {
        GraphWriter writer(brain.behaviour_graph);
        writer.node(1, "ai.root");
        writer.node(2, "ai.selector");
        writer.node(3, "ai.sequence");
        writer.node(4, "ai.condition");
        writer.prop(4, "task", text_literal("contact"));
        writer.node(5, "ai.task");
        writer.prop(5, "task", text_literal("engage"));
        writer.node(6, "ai.task");
        writer.prop(6, "task", text_literal("advance"));
        writer.link(2, "node", 1, "child");
        writer.link(3, "node", 2, "children");
        writer.link(4, "node", 3, "children");
        writer.link(5, "node", 3, "children");
        writer.link(6, "node", 2, "children");
        if (Status written = writer.result(); !written) {
            return written;
        }
        brain.behaviour_graph.resolve(brain.registry);
        Expected<cy::graph::behaviour::BehaviourProgram, Error> compiled =
            cy::graph::behaviour::compile_behaviour(brain.behaviour_graph, brain.registry,
                                                    brain.sink);
        if (!compiled) {
            return Status{cy::make_unexpected(compiled.error())};
        }
        brain.behaviour =
            new (std::nothrow) cy::graph::behaviour::BehaviourProgram(std::move(*compiled));
        ++compilations_;
        report_.behaviour_digest = brain.behaviour == nullptr ? 0ULL : brain.behaviour->digest();
    }

    // --- The locomotion graph: a walk state, an aim layer masked to the arm, and a second state.
    {
        GraphWriter writer(brain.pose_graph);
        writer.node(1, "pose.clip");
        writer.prop(1, "clip", text_literal("walk"));
        writer.prop(1, "time_parameter", text_literal("walk_time"));
        writer.node(2, "pose.clip");
        writer.prop(2, "clip", text_literal("aim"));
        writer.prop(2, "time_parameter", text_literal("aim_time"));
        writer.node(3, "pose.layer");
        writer.prop(3, "mask_first", integer_literal(kJointShoulder));
        writer.prop(3, "mask_count", integer_literal(4));
        writer.prop(3, "weight_parameter", text_literal("aim_weight"));
        writer.node(4, "pose.state");
        writer.node(5, "pose.clip");
        writer.prop(5, "clip", text_literal("aim"));
        writer.prop(5, "time_parameter", text_literal("idle_time"));
        writer.node(6, "pose.state");
        writer.node(7, "pose.transition");
        writer.prop(7, "condition", text_literal("halt"));
        writer.prop(7, "duration", number_literal(0.2F));
        writer.prop(7, "interruption", text_literal("none"));
        writer.link(1, "pose", 3, "a");
        writer.link(2, "pose", 3, "b");
        writer.link(3, "pose", 4, "pose");
        writer.link(5, "pose", 6, "pose");
        writer.link(4, "pose", 7, "from");
        writer.link(6, "pose", 7, "to");
        if (Status written = writer.result(); !written) {
            return written;
        }
        brain.pose_graph.resolve(brain.registry);
        Expected<cy::graph::pose::PoseProgram, Error> compiled = cy::graph::pose::compile_pose(
            brain.pose_graph, brain.registry, kJointCount, brain.sink);
        if (!compiled) {
            return Status{cy::make_unexpected(compiled.error())};
        }
        brain.posed = new (std::nothrow) cy::graph::pose::PoseProgram(std::move(*compiled));
        ++compilations_;
        report_.pose_digest = brain.posed == nullptr ? 0ULL : brain.posed->digest();
    }

    // --- The ability graph: a check stage that can refuse, and a commit stage that emits.
    {
        GraphWriter writer(brain.ability_graph);
        writer.node(1, "ability.stage");
        writer.prop(1, "stage", text_literal("check_state"));
        writer.node(2, "script.const_int");
        writer.prop(2, "value", integer_literal(1));
        writer.node(3, "script.query");
        writer.prop(3, "query", text_literal("ability.has_tag"));
        writer.node(4, "script.branch");
        writer.node(5, "ability.refuse");
        writer.prop(5, "reason", text_literal("suppressed"));
        writer.node(6, "ability.stage");
        writer.prop(6, "stage", text_literal("commit"));
        writer.node(7, "script.emit_command");
        writer.prop(7, "command", text_literal("volley"));
        writer.link(2, "value", 3, "arg0");
        writer.link(1, "then", 4, "in");
        writer.link(3, "value", 4, "condition");
        writer.link(4, "then", 5, "in");
        writer.link(6, "then", 7, "in");
        if (Status written = writer.result(); !written) {
            return written;
        }
        brain.ability_graph.resolve(brain.registry);
        Expected<cy::graph::script::AbilityProgram, Error> compiled =
            cy::graph::script::compile_ability(brain.ability_graph, brain.registry, brain.sink);
        if (!compiled) {
            return Status{cy::make_unexpected(compiled.error())};
        }
        brain.ability = new (std::nothrow) cy::graph::script::AbilityProgram(std::move(*compiled));
        ++compilations_;
        report_.ability_digest =
            brain.ability == nullptr ? 0ULL : brain.ability->program().digest();
    }

    // --- The objective script: `visual-scripting`'s own back end, run once a tick.
    {
        GraphWriter writer(brain.objective_graph);
        writer.node(1, "script.entry");
        writer.node(2, "script.query");
        writer.prop(2, "query", text_literal("objective.progress"));
        writer.node(3, "script.const_float");
        writer.prop(3, "value", number_literal(1.0F));
        writer.node(4, "script.less_float");
        writer.node(5, "script.branch");
        writer.node(6, "script.emit_command");
        writer.prop(6, "command", text_literal("hold"));
        writer.node(7, "script.emit_command");
        writer.prop(7, "command", text_literal("captured"));
        writer.node(8, "script.return");
        writer.link(2, "value", 4, "a");
        writer.link(3, "value", 4, "b");
        writer.link(1, "then", 5, "in");
        writer.link(4, "value", 5, "condition");
        writer.link(5, "then", 6, "in");
        writer.link(5, "else", 7, "in");
        writer.link(6, "then", 8, "in");
        writer.link(7, "then", 8, "in");
        if (Status written = writer.result(); !written) {
            return written;
        }
        brain.objective_graph.resolve(brain.registry);
        cy::graph::script::ScriptCompileOptions script_options;
        Expected<cy::graph::script::ScriptProgram, Error> compiled =
            cy::graph::script::compile_script(brain.objective_graph, brain.registry, script_options,
                                              brain.sink);
        if (!compiled) {
            return Status{cy::make_unexpected(compiled.error())};
        }
        brain.objective = new (std::nothrow) cy::graph::script::ScriptProgram(std::move(*compiled));
        ++compilations_;
        if (brain.objective == nullptr) {
            return cy::fail(cy::ErrorCode::OutOfMemory, "the objective program did not allocate");
        }
        brain.objective_state =
            new (std::nothrow) cy::graph::script::ScriptState(*allocator_, *brain.objective);
        report_.script_digest = brain.objective->digest();
    }

    // --- The camera rig: a follow rig with a collision query and a half-life smoother.
    {
        GraphWriter writer(brain.rig_graph);
        writer.node(1, "camera.input");
        writer.prop(1, "name", text_literal("eye"));
        writer.prop(1, "type", text_literal("vector"));
        writer.node(2, "camera.input");
        writer.prop(2, "name", text_literal("target"));
        writer.prop(2, "type", text_literal("vector"));
        writer.node(3, "camera.input");
        writer.prop(3, "name", text_literal("state"));
        writer.prop(3, "type", text_literal("vector"));
        writer.node(4, "camera.collide");
        writer.node(5, "camera.parameter");
        writer.prop(5, "name", text_literal("half_life"));
        writer.node(6, "camera.input");
        writer.prop(6, "name", text_literal("dt"));
        writer.node(7, "camera.smooth_half_life");
        writer.node(8, "camera.parameter");
        writer.prop(8, "name", text_literal("focal"));
        writer.node(9, "camera.lens");
        writer.node(10, "camera.output");
        writer.link(1, "value", 4, "origin");
        writer.link(2, "value", 4, "target");
        writer.link(3, "value", 7, "previous");
        writer.link(4, "value", 7, "desired");
        writer.link(5, "value", 7, "half_life");
        writer.link(6, "value", 7, "dt");
        writer.link(8, "value", 9, "focal_length");
        writer.link(7, "value", 10, "pose");
        writer.link(9, "value", 10, "lens");
        writer.link(7, "value", 10, "state");
        if (Status written = writer.result(); !written) {
            return written;
        }
        brain.rig_graph.resolve(brain.registry);
        Expected<cy::graph::camera::CameraRigProgram, Error> compiled =
            cy::graph::camera::compile_rig(brain.rig_graph, brain.registry, brain.sink);
        if (!compiled) {
            return Status{cy::make_unexpected(compiled.error())};
        }
        brain.rig_program =
            new (std::nothrow) cy::graph::camera::CameraRigProgram(std::move(*compiled));
        ++compilations_;
        if (brain.rig_program != nullptr) {
            report_.rig_digest = brain.rig_program->digest();
            report_.rig_ir_digest = brain.rig_program->ir_digest();
        }
    }

    if (brain.sink.errors() != 0U) {
        return cy::fail(cy::ErrorCode::InvalidArgument,
                        "one of the slice's authored graphs did not compile");
    }
    if (brain.behaviour == nullptr || brain.posed == nullptr || brain.ability == nullptr ||
        brain.rig_program == nullptr || brain.objective_state == nullptr) {
        return cy::fail(cy::ErrorCode::OutOfMemory, "a compiled program did not allocate");
    }

    // --- The animation rig the pose program is bound to.
    if (Status skeleton = build_biped(brain.skeleton); !skeleton) {
        return skeleton;
    }
    if (Status walk = build_walk(brain.walk, 1.6F); !walk) {
        return walk;
    }
    if (Status aim = build_aim(brain.aim); !aim) {
        return aim;
    }
    if (Status sized = brain.clip_table.resize(brain.posed->clips().size()); !sized) {
        return sized;
    }
    for (cy::usize index = 0; index < brain.posed->clips().size(); ++index) {
        brain.clip_table[index] =
            brain.posed->clips()[index].name == Name::intern("aim") ? &brain.aim : &brain.walk;
    }
    if (Status bound = brain.rig.bind(brain.skeleton, *brain.posed, brain.clip_table.span());
        !bound) {
        return bound;
    }
    if (Status prepared = brain.scratch.prepare(brain.rig); !prepared) {
        return prepared;
    }
    return brain.poses.resize(static_cast<cy::usize>(kEvaluatedPoses) * kJointCount);
}

// --- The gameplay framework, and the ability the squad carries
// ------------------------------------

Status Slice::build_abilities() noexcept {
    Kit& kit = *kit_;

    // Phases: a warm-up the session starts in, a play phase, and an abort reachable from anywhere.
    Expected<cy::gameplay::TagId, Error> warmup = kit.tags.declare("Match.Warmup");
    Expected<cy::gameplay::TagId, Error> play = kit.tags.declare("Match.Play");
    Expected<cy::gameplay::TagId, Error> abort = kit.tags.declare("Match.Abort");
    Expected<cy::gameplay::TagId, Error> cue = kit.tags.declare("Cue.Volley.Fire");
    Expected<cy::gameplay::TagId, Error> suppressed = kit.tags.declare("State.Suppressed");
    if (!warmup || !play || !abort || !cue || !suppressed) {
        return cy::fail(cy::ErrorCode::Internal, "the session's tags could not be declared");
    }
    kit.cue = *cue;
    kit.suppressed = *suppressed;
    kit.phase_play = *play;
    // `allow(kInvalidTag, to)` is THE INITIAL STATE and not a wildcard; `allow_from_any` is the
    // wildcard, and the two have opposite consequences for a phase nothing should return to.
    if (Status allowed = kit.phases.allow(cy::gameplay::kInvalidTag, *warmup); !allowed) {
        return allowed;
    }
    if (Status allowed = kit.phases.allow(*warmup, *play); !allowed) {
        return allowed;
    }
    if (Status allowed = kit.phases.allow_from_any(*abort); !allowed) {
        return allowed;
    }
    if (!kit.phases.enter(*warmup, 0).permitted()) {
        return cy::fail(cy::ErrorCode::Internal, "the session could not enter its first phase");
    }

    // Two teams, hostile to each other. `Relationship::Hostile` is what the targeting rules read.
    Expected<cy::gameplay::TeamId, Error> blue = kit.relationships.add_team(Name::intern("blue"));
    Expected<cy::gameplay::TeamId, Error> red = kit.relationships.add_team(Name::intern("red"));
    if (!blue || !red) {
        return cy::fail(cy::ErrorCode::Internal, "the session's teams could not be declared");
    }
    kit.blue = *blue;
    kit.red = *red;
    if (Status set =
            kit.relationships.set_relationship(*blue, *red, cy::gameplay::Relationship::Hostile);
        !set) {
        return set;
    }

    AttributeDeclaration health;
    health.name = Name::intern("health");
    health.stable_id = 1;
    health.base = 100.0F;
    Expected<cy::gameplay::abilities::AttributeId, Error> declared_health =
        kit.schema.declare(health);
    AttributeDeclaration energy;
    energy.name = Name::intern("energy");
    energy.stable_id = 2;
    energy.base = 100.0F;
    Expected<cy::gameplay::abilities::AttributeId, Error> declared_energy =
        kit.schema.declare(energy);
    if (!declared_health || !declared_energy) {
        return cy::fail(cy::ErrorCode::Internal, "the attribute schema could not be declared");
    }
    kit.health = *declared_health;
    kit.energy = *declared_energy;

    EffectDefinition burn;
    burn.name = Name::intern("burn");
    burn.stable_id = 1;
    burn.kind = EffectKind::Periodic;
    burn.duration_ticks = 100000;
    burn.period_ticks = 10;
    burn.stacking = StackingPolicy::Stack;
    const EffectModifier held{kit.health, ModifierOp::Add, -1.0F, 0, Name{}};
    const EffectModifier per_tick{kit.health, ModifierOp::Add, -1.0F, 0, Name{}};
    Expected<EffectDefId, Error> declared_burn = kit.effects.declare(
        burn, Span<const EffectModifier>(&held, 1), Span<const EffectModifier>(&per_tick, 1));
    if (!declared_burn) {
        return Status{cy::make_unexpected(declared_burn.error())};
    }
    kit.burn = *declared_burn;

    AbilityDefinition volley;
    volley.name = Name::intern("volley");
    volley.stable_id = 1;
    volley.cooldown_ticks = 30;
    volley.targeting.max_range = 30.0F;
    volley.program = brain_->ability;
    const Cost cost{CostKind::Attribute, kit.energy, 5.0F, Name{}};
    Expected<cy::gameplay::abilities::AbilityId, Error> declared_volley = kit.abilities.declare(
        volley, Span<const Cost>(&cost, 1), Span<const EffectDefId>(&kit.burn, 1));
    if (!declared_volley) {
        return Status{cy::make_unexpected(declared_volley.error())};
    }
    kit.volley = *declared_volley;
    return kit.effects.reserve(options_.effects + options_.agents + 16U);
}

// --- The characters
// --------------------------------------------------------------------------------

Status Slice::build_characters() noexcept {
    Level& level = *level_;
    Brain& brain = *brain_;
    Kit& kit = *kit_;
    const u32 count = options_.agents;

    brain.batch = new (std::nothrow) cy::animation::AnimationBatch(*allocator_, brain.rig);
    // THE THINK BUDGET IS PER TIER AND THE DEFAULT ONE IS NOT THIS GAME'S. `AiBudget` defaults to
    // 2000/2000/1000/200 thinks a tick, which at eight thousand agents defers most of the
    // `Minimal` and `Statistical` populations every tick and STARVES them — `ThinkReport::starved`
    // is "agents that have not thought within their tier's guaranteed interval. Zero is the
    // requirement; a gate reads this and nothing else". What bounds this game's thinking is the
    // TIER RATE (a `Statistical` agent thinks once in sixteen ticks), so the per-tier budget is set
    // above the population and the rotation is what does the work.
    cy::ai::AiBudget budget;
    for (cy::u32& thinks : budget.thinks_per_tick) {
        thinks = count + 1U;
    }

    // ==========================================================================================
    // THE AI LOD POLICY IS THE CONFIGURATION THE EXIT CRITERION MEANS
    // ==========================================================================================
    //
    // "Cost is bounded by configuration: 8,000 agents and 100 concurrent effects hold their
    // budgets." The dial that bounds AI cost is the tier policy, and the DEFAULT one is written
    // for a world tens of kilometres across: `full_distance` 30 m, `reduced_distance` 80 m. This
    // arena is 48 m on a side, so under the default policy every one of eight thousand agents is
    // Full — the whole crowd senses and thinks every tick, and the cost is the configuration's
    // rather than the engine's.
    //
    // These distances are the same policy expressed for THIS level: a fighting core near the
    // objective, a reduced ring around it, and a statistical crowd beyond. `Minimal` and
    // `Statistical` agents run no sensors at all (`PerceptionScheduler::gather` skips them by
    // name), which is what makes the sensing cost a function of the ring rather than of the crowd.
    // `TierReport::by_tier` is printed, so the distribution is visible rather than asserted.
    cy::ai::TierPolicy tiers;
    tiers.full_distance = 7.0F;
    tiers.reduced_distance = 14.0F;
    tiers.minimal_distance = 22.0F;
    tiers.hysteresis = 2.0F;
    brain.runtime = new (std::nothrow) cy::ai::AiRuntime(*allocator_, budget, tiers);
    cy::ai::PerceptionParams perception;
    perception.query_budget = 512;
    brain.perception = new (std::nothrow) cy::ai::PerceptionScheduler(*allocator_, perception);
    if (brain.batch == nullptr || brain.runtime == nullptr || brain.perception == nullptr) {
        return cy::fail(cy::ErrorCode::OutOfMemory, "the runtimes did not allocate");
    }
    Expected<u32, Error> first_slot = brain.runtime->reserve_slots(*brain.behaviour, count);
    if (!first_slot) {
        return Status{cy::make_unexpected(first_slot.error())};
    }

    cy::ai::KnowledgeParams knowledge;
    knowledge.capacity = 8;
    cy::navigation::AvoidanceParams avoidance;
    avoidance.radius = kCharacterRadius;
    avoidance.height = 1.8F;
    avoidance.max_speed = 3.2F;

    for (u32 index = 0; index < count; ++index) {
        // A double spiral, so the two teams start apart and meet in the middle.
        const bool red_team = (index % 2U) == 1U;
        const f32 angle = static_cast<f32>(index) * 0.61803F;
        // Spread over the DISC, not over a ring: the square root is what makes the density even,
        // so a run at eight thousand is the same crowd as a run at two thousand over four times
        // the floor rather than a much tighter one.
        const f32 spread = (level.half_extent - 6.0F);
        const f32 ring =
            3.0F + (std::sqrt(static_cast<f32>(index) / static_cast<f32>(count)) * spread);
        const f32 side = red_team ? -1.0F : 1.0F;
        const Vec3 at{ring * std::cos(angle) * side, 0.0F, ring * std::sin(angle) * side};

        Expected<Entity, Error> entity = level.place(
            at, MeshKind::Character, red_team ? MaterialKind::TeamRed : MaterialKind::TeamBlue,
            Vec3{0.35F, 0.9F, 0.35F});
        if (!entity) {
            return Status{cy::make_unexpected(entity.error())};
        }
        if (Status pushed = characters_.entities.push_back(*entity); !pushed) {
            return pushed;
        }
        if (Status pushed = characters_.positions.push_back(at); !pushed) {
            return pushed;
        }
        if (Status pushed = characters_.forward.push_back(Vec3{0.0F, 0.0F, -1.0F}); !pushed) {
            return pushed;
        }
        if (Status pushed = characters_.goals.push_back(Vec3{-at.x, 0.0F, -at.z}); !pushed) {
            return pushed;
        }
        if (Status pushed = characters_.team.push_back(red_team ? cy::u8{1} : cy::u8{0}); !pushed) {
            return pushed;
        }

        Expected<cy::navigation::CrowdAgentId, Error> crowd = brain.crowd.add(at, avoidance);
        if (!crowd) {
            return Status{cy::make_unexpected(crowd.error())};
        }
        if (Status pushed = characters_.crowd.push_back(*crowd); !pushed) {
            return pushed;
        }

        cy::ai::AIAgent agent;
        agent.graph = Name::intern("squad");
        agent.importance = 1.0F + static_cast<f32>(index % 3U);
        agent.seed = options_.seed ^ static_cast<u64>(index);
        if (Status pushed = brain.agents.push_back(agent); !pushed) {
            return pushed;
        }
        cy::ai::AIState state;
        state.slot = *first_slot + index;
        if (Status pushed = brain.states.push_back(state); !pushed) {
            return pushed;
        }
        if (Status pushed = brain.blackboards.push_back(cy::ai::Blackboard{}); !pushed) {
            return pushed;
        }
        cy::ai::PerceptionSensors sensors;
        sensors.own_faction = red_team ? 2U : 1U;
        sensors.factions_of_interest = red_team ? 1U : 2U;
        // A CROWD MEMBER'S EYES ARE THE CROWD'S, NOT A COMBATANT'S. The default sensor sees 20 m,
        // which in a 48 m arena is most of the level: every agent then has a line to every target
        // and the scheduler's broad phase rejects nothing. The squad — the first
        // `kSquad` characters, which are also the perception targets — keeps the default; the
        // crowd behind them sees nine metres, which is the same decision a game makes when it
        // decides what a background character notices.
        if (index >= kSquad) {
            sensors.sight_range = 9.0F;
            sensors.field_of_view_degrees = 90.0F;
            sensors.hearing_range = 6.0F;
        }
        if (Status pushed = brain.sensors.push_back(sensors); !pushed) {
            return pushed;
        }
        if (Status pushed = brain.importance.push_back(agent.importance); !pushed) {
            return pushed;
        }
        if (Status pushed = brain.tiers.push_back(cy::ai::AiTier::Full); !pushed) {
            return pushed;
        }
        if (Status pushed =
                brain.knowledge.push_back(cy::ai::KnowledgeStore(*allocator_, knowledge));
            !pushed) {
            return pushed;
        }

        Expected<u32, Error> slot = brain.batch->add();
        if (!slot) {
            return Status{cy::make_unexpected(slot.error())};
        }
        if (Status pushed = characters_.animation_slot.push_back(*slot); !pushed) {
            return pushed;
        }
        cy::animation::AnimationInstance& instance = brain.batch->instance(*slot);
        instance.set_identifier(index);
        // A spread of play rates, so the crowd does not march in lockstep and the pose cache's
        // phase buckets have something to bucket.
        instance.set_play_rate(0.85F + (static_cast<f32>(index % 7U) * 0.05F));
        if (Status parameter = instance.set_parameter(brain.rig, Name::intern("aim_weight"),
                                                      red_team ? 1.0F : 0.0F);
            !parameter) {
            return parameter;
        }

        if (Status added = kit.attributes.add_entity(*entity); !added) {
            return added;
        }
        // `grant_ability` answers with the GRANT's own identity, not with a bare success: a
        // revocation names the grant rather than the ability, so a character who was given the
        // same ability twice loses only the grant that is taken back.
        const Expected<cy::gameplay::abilities::GrantId, Error> granted =
            kit.abilities.grant_ability(*entity, kit.volley, Name::intern("kit"), 0);
        if (!granted) {
            return cy::Unexpected<Error>(granted.error());
        }
        if (Status team = kit.relationships.set_team(*entity, red_team ? kit.red : kit.blue);
            !team) {
            return team;
        }
    }
    for (u32 index = 0; index < count; ++index) {
        if (Status pushed = brain.stores.push_back(&brain.knowledge[index]); !pushed) {
            return pushed;
        }
    }

    // The perception targets: the far team's leaders, capped so the broad phase has something to
    // reject rather than a target per character per tick.
    const u32 target_count = count < kSquad ? count : kSquad;
    for (u32 index = 0; index < target_count; ++index) {
        cy::ai::PerceptionTarget target;
        target.entity = characters_.entities[index];
        target.position = characters_.positions[index];
        target.faction = characters_.team[index] == 1U ? 2U : 1U;
        target.relevance = 1.0F;
        if (Status pushed = brain.targets.push_back(target); !pushed) {
            return pushed;
        }
    }

    // The hundred concurrent gameplay effects the exit criterion names, lit at build time so the
    // measured loop measures them from its first tick.
    const u32 lit = options_.effects < count ? options_.effects : count;
    for (u32 index = 0; index < lit; ++index) {
        ApplyReport applied;
        if (Status lit_one = kit.effects.apply(kit.burn, characters_.entities[index],
                                               characters_.entities[0], 0, &kit.tag_store, applied);
            !lit_one) {
            return lit_one;
        }
    }

    // The targeting context: this module has no world, so the level answers for it.
    TargetWorld::positions = characters_.positions.span();
    kit.target_context.position = &TargetWorld::position;
    kit.target_context.line_of_sight = &TargetWorld::visible;
    kit.pipeline.set_target_context(kit.target_context);
    kit.pipeline.set_target_buffer(&kit.buffer);
    if (Status reserved = kit.pipeline.reserve(count + 64U, 256U); !reserved) {
        return reserved;
    }

    if (options_.interpret_control) {
        // THE NEGATIVE CONTROL. One interpreted node per character, which is what the audit exists
        // to find. See `InterpretedNode`.
        for (u32 index = 0; index < count; ++index) {
            auto* node = new (std::nothrow) WanderNode();
            if (node == nullptr) {
                return cy::fail(cy::ErrorCode::OutOfMemory, "the control could not allocate");
            }
            if (Status pushed = brain.interpreted.push_back(node); !pushed) {
                return pushed;
            }
        }
    }
    return cy::ok();
}

}  // namespace cy::sample::slice
