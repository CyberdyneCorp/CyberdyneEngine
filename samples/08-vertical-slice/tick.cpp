// The loop, the audit and the picture. M8.b section 12.
//
// `Slice::tick()` is the milestone in one screen: six batch calls and a frame, over one set of
// packed columns. Every function above it is one of those calls and nothing else.

#include "internals.h"

#include <cy/core/determinism/commit.h>
#include <cy/graph/audit.h>

#include "capture.h"
#include "presentation.h"
#include "spectacle.h"

#include <type_traits>

namespace cy::sample::slice {
namespace {

using cy::gameplay::abilities::ActivationReport;
using cy::gameplay::abilities::ActivationRequest;
using cy::gameplay::abilities::EffectTickReport;
using cy::gameplay::abilities::TargetKind;

[[nodiscard]] Vec3 towards(Vec3 from, Vec3 to, f32 speed) noexcept {
    const f32 dx = to.x - from.x;
    const f32 dz = to.z - from.z;
    const f32 length = std::sqrt((dx * dx) + (dz * dz));
    if (length < 0.05F) {
        return Vec3{0.0F, 0.0F, 0.0F};
    }
    return Vec3{(dx / length) * speed, 0.0F, (dz / length) * speed};
}

}  // namespace

// --- Think
// ------------------------------------------------------------------------------------------

Status Slice::think(TickCosts& costs) noexcept {
    Brain& brain = *brain_;
    const auto tick = static_cast<u32>(tick_);

    cy::ai::TierReport tiers;
    const Vec3 observer{0.0F, 1.6F, 0.0F};
    if (Status decided =
            brain.runtime->update_tiers(brain.agents.span(), characters_.positions.span(),
                                        brain.states.span(), {&observer, 1}, tiers);
        !decided) {
        return decided;
    }
    for (u32 index = 0; index < characters_.size(); ++index) {
        brain.tiers[index] = brain.agents[index].tier;
    }
    for (u32 tier = 0; tier < 4U; ++tier) {
        report_.agents_at_tier[tier] = tiers.by_tier[tier];
    }

    ArenaVisibility eyes(level_->pillars.span(), 0.7F);
    cy::ai::PerceptionReport sensed;
    const f64 sense_started = cpu_micros();
    const cy::ai::ObserverColumns columns{characters_.entities.span(), characters_.positions.span(),
                                          characters_.forward.span(),  brain.sensors.span(),
                                          brain.importance.span(),     brain.tiers.span(),
                                          brain.stores.span()};
    for (cy::usize index = 0; index < brain.targets.size(); ++index) {
        brain.targets[index].position = characters_.positions[index];
    }
    if (Status looked = brain.perception->update(tick, columns, brain.targets.span(), eyes, sensed);
        !looked) {
        return looked;
    }
    costs.sense_us = cpu_micros() - sense_started;
    report_.perception_queries += sensed.queries_issued;

    brain.host.contact = sensed.sightings > 0U;
    brain.host.objective_live = kit_->phases.current() == kit_->phase_play;
    brain.host.engage_score = sensed.sightings > 0U ? 0.8F : 0.2F;

    cy::ai::ThinkReport thought;
    const f64 think_started = cpu_micros();
    if (Status decided =
            brain.runtime->think(tick, *brain.behaviour, characters_.entities.span(),
                                 brain.agents.span(), brain.states.span(), brain.blackboards.span(),
                                 brain.stores.span(), brain.host, kDeltaTime, thought);
        !decided) {
        return decided;
    }
    costs.think_us = cpu_micros() - think_started;
    report_.think_instructions += thought.instructions;
    report_.agents_thought +=
        thought.thought[0] + thought.thought[1] + thought.thought[2] + thought.thought[3];
    report_.agents_starved += thought.starved;
    fold(thought.state_hash);

    // THE NEGATIVE CONTROL, ticked where a real interpreter would be. It changes nothing the game
    // reads; what it changes is what `audit()` finds.
    for (InterpretedNode* node : brain.interpreted.span()) {
        node->tick(kDeltaTime);
    }
    return cy::ok();
}

// --- Navigate ------------------------------------------------------------------------------------

Status Slice::navigate(TickCosts& costs) noexcept {
    Brain& brain = *brain_;
    Level& level = *level_;
    const f64 started = cpu_micros();

    // A handful of real path queries a tick, rotating through the squad: the crowd steers, and the
    // navigation mesh is what says where "towards the objective" actually leads.
    cy::navigation::PathCorridor corridor(*allocator_);
    const cy::navigation::PathFilter filter;
    const u32 queried = characters_.size() < 4U ? characters_.size() : 4U;
    for (u32 which = 0; which < queried; ++which) {
        const u32 index = static_cast<u32>((tick_ * queried) + which) % characters_.size();
        corridor.clear();
        const cy::navigation::PathResult found = cy::navigation::find_path(
            level.mesh, characters_.positions[index], characters_.goals[index],
            Vec3{2.0F, 2.0F, 2.0F}, filter, corridor);
        if (found.found) {
            ++report_.paths_found;
        }
    }

    for (u32 index = 0; index < characters_.size(); ++index) {
        // What the character is steering at: whatever perception knows about, or its objective.
        Vec3 goal = characters_.goals[index];
        const cy::ai::KnowledgeEntry* known = brain.knowledge[index].best_target();
        if (known != nullptr) {
            goal = known->last_position;
        }
        const Vec3 desired = towards(characters_.positions[index], goal, 3.0F);
        brain.crowd.set_desired_velocity(characters_.crowd[index], desired);
        // THE CROWD TIER FOLLOWS THE AI TIER, ALL THREE OF THEM. A `Minimal` or `Statistical`
        // agent runs no sensors and thinks at the slowest rate; giving it a Reduced avoidance
        // solve anyway would put the whole crowd's cost back and leave the LOD policy deciding
        // only half of what it decides.
        cy::navigation::CrowdTier crowd_tier = cy::navigation::CrowdTier::Minimal;
        if (brain.tiers[index] == cy::ai::AiTier::Full) {
            crowd_tier = cy::navigation::CrowdTier::Full;
        } else if (brain.tiers[index] == cy::ai::AiTier::Reduced) {
            crowd_tier = cy::navigation::CrowdTier::Reduced;
        }
        brain.crowd.set_tier(characters_.crowd[index], crowd_tier);
    }

    cy::navigation::CrowdReport crowd_report;
    if (Status stepped = brain.crowd.step(kDeltaTime, crowd_report); !stepped) {
        return stepped;
    }
    brain.crowd.integrate(kDeltaTime);
    report_.crowd_adjusted += crowd_report.agents_adjusted;

    for (u32 index = 0; index < characters_.size(); ++index) {
        const cy::navigation::CrowdAgent* agent = brain.crowd.agent(characters_.crowd[index]);
        if (agent == nullptr) {
            continue;
        }
        characters_.positions[index] = agent->position;
        const f32 speed = std::sqrt((agent->velocity.x * agent->velocity.x) +
                                    (agent->velocity.z * agent->velocity.z));
        if (speed > 0.05F) {
            characters_.forward[index] =
                Vec3{agent->velocity.x / speed, 0.0F, agent->velocity.z / speed};
        }
        if (Status moved = level.move(characters_.entities[index], agent->position); !moved) {
            return moved;
        }
        // The character reached its objective: turn round. A game, not a benchmark.
        const f32 dx = characters_.goals[index].x - agent->position.x;
        const f32 dz = characters_.goals[index].z - agent->position.z;
        if ((dx * dx) + (dz * dz) < 1.0F) {
            characters_.goals[index] =
                Vec3{-characters_.goals[index].x, 0.0F, -characters_.goals[index].z};
        }
    }
    costs.navigate_us = cpu_micros() - started;
    return cy::ok();
}

// --- Animate
// --------------------------------------------------------------------------------------

Status Slice::animate(TickCosts& costs) noexcept {
    Brain& brain = *brain_;
    const f64 started = cpu_micros();

    // ADVANCE EVERY INSTANCE. Root motion is integrated here and not in the evaluator, which is the
    // determinism contract: a tier may skip a pose and may not skip a metre.
    if (Status advanced = brain.batch->advance_all(kDeltaTime, nullptr); !advanced) {
        return advanced;
    }

    // EVALUATE A BOUNDED SET. `animation-and-skinning`'s LOD is exactly this: the poses somebody
    // looks at, and no more.
    const u32 count = characters_.size() < kEvaluatedPoses ? characters_.size() : kEvaluatedPoses;
    if (count > 0U) {
        const u32 first = static_cast<u32>(tick_ * count) % characters_.size();
        const u32 window =
            (first + count) <= characters_.size() ? count : characters_.size() - first;
        for (Transform& pose : brain.poses) {
            pose = Transform::identity();
        }
        cy::animation::EvaluationStats stats;
        if (Status evaluated = brain.batch->evaluate_range(
                first, window, 0, brain.scratch,
                Span<Transform>(brain.poses.data(), static_cast<cy::usize>(window) * kJointCount),
                stats);
            !evaluated) {
            return evaluated;
        }
        report_.clips_sampled += stats.clips_sampled;
        report_.joints_sampled += stats.joints_sampled;
        report_.poses_evaluated += window;
    }

    const cy::animation::AnimationInstance& first_instance = brain.batch->instance(0);
    const Vec3 travelled = first_instance.travelled();
    report_.root_motion_travelled =
        std::sqrt((travelled.x * travelled.x) + (travelled.z * travelled.z));
    costs.animate_us = cpu_micros() - started;
    fold(quantise(report_.root_motion_travelled));
    return cy::ok();
}

// --- Act
// -------------------------------------------------------------------------------------------

/// One committed activation's effect, at the caster. A function rather than two copies of a null
/// check because the rule — an effect per committed activation — has two call sites and a rule with
/// two spellings is two rules.
Status Slice::play_cue(u32 character) noexcept {
    if (spectacle_ == nullptr || character >= characters_.size()) {
        return cy::ok();
    }
    return spectacle_->on_cue(characters_.positions[character]);
}

Status Slice::act(TickCosts& costs) noexcept {
    Kit& kit = *kit_;
    const f64 started = cpu_micros();

    // A rotating slice of the squad fires each tick. The pipeline runs the specification's order —
    // cost before cooldown — and the compiled ability program's check stage runs inside it.
    const u32 firing = characters_.size() < 8U ? characters_.size() : 8U;
    cy::determinism::SimulationPoint point;
    point.tick = tick_;
    for (u32 which = 0; which < firing; ++which) {
        const u32 index = static_cast<u32>((tick_ * firing) + which) % characters_.size();
        const u32 victim = (index + (characters_.size() / 2U) + 1U) % characters_.size();
        if (victim == index) {
            continue;
        }
        ActivationRequest request;
        request.owner = characters_.entities[index];
        request.ability = kit.volley;
        request.target.kind = TargetKind::Entity;
        request.target.entity = characters_.entities[victim];
        request.cue = kit.cue;
        ActivationReport activation;
        Expected<cy::gameplay::abilities::ActivationId, Error> id =
            kit.pipeline.activate(request, point, activation);
        if (!id) {
            return Status{cy::make_unexpected(id.error())};
        }
        if (activation.committed) {
            ++report_.activations_committed;
            // THE VFX SEAM M8.b's README NAMED, USED. `ActivationPipeline` emits a cue per
            // committed activation and M8.b counted them; M8.c plays an effect at the caster
            // instead. Nothing about the activation changes: the cue is still emitted, still
            // suppressed by (activation, cue, simulation point), and still folded below.
            if (Status played = play_cue(index); !played) {
                return played;
            }
        } else {
            ++report_.activations_refused;
        }
        report_.cues_emitted += activation.cues_emitted;
        report_.cues_suppressed += activation.cues_suppressed;
        fold(id->bits);
    }

    // AND ONE THAT MUST BE REFUSED, every tick. The rotation above refuses an activation only when
    // it happens to come round to a caster still on cooldown, which depends on the squad's size —
    // so the artefact's "refusals are refusals" claim would hold at one population and not at the
    // next. This fires the character that just fired, again, in the same tick:
    // `ability-and-effects` checks cost before cooldown, so it is refused for one of the two, and
    // the pipeline having committed nothing is what the counter records.
    if (firing > 0U && characters_.size() > 1U) {
        const u32 index = static_cast<u32>(tick_ * firing) % characters_.size();
        const u32 victim = (index + 1U) % characters_.size();
        ActivationRequest again;
        again.owner = characters_.entities[index];
        again.ability = kit.volley;
        again.target.kind = TargetKind::Entity;
        again.target.entity = characters_.entities[victim];
        again.cue = kit.cue;
        ActivationReport repeated;
        Expected<cy::gameplay::abilities::ActivationId, Error> id =
            kit.pipeline.activate(again, point, repeated);
        if (!id) {
            return Status{cy::make_unexpected(id.error())};
        }
        if (repeated.committed) {
            ++report_.activations_committed;
            // AND THIS ONE GETS AN EFFECT TOO, for the same reason the rotation above does: the
            // rule is "an effect per committed activation" and a second rule for a second call site
            // would be two rules. It matters at the scale act's population, where it is the ONLY
            // activation that commits — see the finding in README.md about the target being half
            // an arena away at 8,000 agents.
            if (Status played = play_cue(index); !played) {
                return played;
            }
        } else {
            ++report_.activations_refused;
        }
        fold(id->bits);
    }

    EffectTickReport effects;
    const f64 effects_started = cpu_micros();
    kit.effects.advance(static_cast<cy::i64>(tick_), effects);
    costs.effects_us = cpu_micros() - effects_started;
    costs.abilities_us = effects_started - started;
    report_.effects_expired += effects.expired;
    const u32 live = kit.effects.active_count();
    report_.effects_peak = live > report_.effects_peak ? live : report_.effects_peak;

    // The objective script, run through `visual-scripting`'s own bytecode back end. One shared
    // program, one instance's state.
    Brain& brain = *brain_;
    brain.objective_host.progress =
        static_cast<f32>(tick_) / static_cast<f32>(options_.ticks == 0U ? 1U : options_.ticks);
    brain.objective_state->set_resume_block(cy::graph::script::kNoBlock);
    Expected<cy::graph::script::RunOutcome, Error> outcome =
        cy::graph::script::execute(*brain.objective, *brain.objective_state, brain.objective_host);
    if (!outcome) {
        return Status{cy::make_unexpected(outcome.error())};
    }

    // The phase advances once, when the warm-up is over. `PhaseController` refuses anything else.
    if (tick_ == 30U && kit.phases.current() != kit.phase_play) {
        (void)kit.phases.enter(kit.phase_play, tick_);
    }

    kit.time.advance(kDeltaTime);
    fold(kit.effects.digest());
    fold(kit.attributes.digest());
    return cy::ok();
}

// --- Present
// ----------------------------------------------------------------------------------------

Status Slice::publish() noexcept {
    Level& level = *level_;
    Brain& brain = *brain_;

    cy::determinism::CommitRecord record;
    record.state_version = tick_ + 1U;
    if (Status extracted = level.extractor->on_commit(record); !extracted) {
        return extracted;
    }

    // The camera rig, compiled and evaluated: one call, and the queries inside it batched into one.
    const Vec3 focus = characters_.size() > 0U ? characters_.positions[0] : Vec3{0.0F, 0.0F, 0.0F};
    const Name input_names[4] = {Name::intern("eye"), Name::intern("target"), Name::intern("state"),
                                 Name::intern("dt")};
    const f32 input_values[4] = {focus.y + 14.0F, focus.z, 0.0F, kDeltaTime};
    const Name parameter_names[2] = {Name::intern("half_life"), Name::intern("focal")};
    const f32 parameter_values[2] = {0.25F, 35.0F};
    cy::graph::camera::RigInputs inputs;
    inputs.input_names = Span<const Name>(input_names, 4);
    inputs.input_values = Span<const f32>(input_values, 4);
    inputs.parameter_names = Span<const Name>(parameter_names, 2);
    inputs.parameter_values = Span<const f32>(parameter_values, 2);
    inputs.dt = kDeltaTime;
    if (Status evaluated = cy::graph::camera::evaluate_rig(
            *brain.rig_program, inputs, brain.rig_instance, brain.rig_queries, brain.rig_output);
        !evaluated) {
        return evaluated;
    }
    report_.rig_query_calls = brain.rig_queries.calls;
    report_.rig_queries = brain.rig_queries.resolved;
    return cy::ok();
}

// --- One tick ------------------------------------------------------------------------------------

/// The effect world and the cinematic, and the view they hand back. `view` goes in carrying the
/// camera the compiled gameplay rig produced and comes out carrying the camera the STACK produced
/// on the frames the cut is driving — see spectacle.h for why that is the whole of task 5.2.
Status Slice::advance_spectacle(const Vec3& subject, ViewState& view, TickCosts& costs) noexcept {
    if (spectacle_ == nullptr) {
        return cy::ok();
    }
    CameraPose gameplay;
    gameplay.eye = view.eye;
    gameplay.target = view.target;
    gameplay.vertical_fov = view.vertical_fov;
    CameraPose chosen;
    if (Status advanced =
            spectacle_->step(static_cast<u32>(tick_), kDeltaTime, subject, gameplay, chosen);
        !advanced) {
        return advanced;
    }
    costs.particles_us = spectacle_->particles_us();
    costs.cinematic_us = spectacle_->cinematic_us();
    view.eye = chosen.eye;
    view.target = chosen.target;
    view.vertical_fov = chosen.vertical_fov;
    return spectacle_->publish(view.eye);
}

Status Slice::tick(TickCosts& costs) noexcept {
    if (Status thought = think(costs); !thought) {
        return thought;
    }
    if (Status moved = navigate(costs); !moved) {
        return moved;
    }
    if (Status posed = animate(costs); !posed) {
        return posed;
    }
    if (Status acted = act(costs); !acted) {
        return acted;
    }
    if (Status published = publish(); !published) {
        return published;
    }

    HudState hud;
    hud.squad_alive = characters_.size();
    hud.contacts = brain_->host.contact ? 1U : 0U;
    hud.health = kit_->attributes.current(characters_.entities[0], kit_->health) / 100.0F;
    hud.energy = kit_->attributes.current(characters_.entities[0], kit_->energy) / 100.0F;
    // The menu opens for a quarter of every two seconds — and on the LAST tick, which is the one
    // the picture is taken on. A screenshot of a menu that is shut is a screenshot of no menu, and
    // nothing the simulation folds into its digest depends on this: the menu is presentation, and
    // `fold_tick` closes over placements and the tick number.
    hud.menu_open = ((tick_ % 120U) < 30U) || ((tick_ + 1U) == options_.ticks);
    if (Status drawn = presentation_->update_interface(hud, report_, costs.interface_us); !drawn) {
        return drawn;
    }
    if (Status menu = presentation_->update_menu(hud.menu_open, report_); !menu) {
        return menu;
    }
    if (Status sound =
            presentation_->update_audio(characters_.positions[0], static_cast<u32>(tick_), report_);
        !sound) {
        return sound;
    }

    if (options_.render) {
        // THE VIEW THE COMPILED RIG PRODUCED, aimed at where the fight actually is. The rig
        // program decides the eye; the centroid of the squad decides what it looks at, which is
        // the same division `camera-system` draws between a rig and its target binding.
        Vec3 centroid{0.0F, 0.9F, 0.0F};
        const u32 sampled = characters_.size() < kSquad ? characters_.size() : kSquad;
        for (u32 index = 0; index < sampled; ++index) {
            centroid.x += characters_.positions[index].x;
            centroid.z += characters_.positions[index].z;
        }
        if (sampled > 0U) {
            centroid.x /= static_cast<f32>(sampled);
            centroid.z /= static_cast<f32>(sampled);
        }
        const f32 back = level_->half_extent * 0.95F;
        ViewState view;
        view.eye =
            Vec3{brain_->rig_output.position[0] + (centroid.x * 0.4F), level_->half_extent * 0.85F,
                 brain_->rig_output.position[2] + centroid.z + back};
        view.target = centroid;
        view.focal_length = brain_->rig_output.focal_length;

        // THE CUT TAKES THE CAMERA, AND IT TAKES IT THROUGH THE CAMERA STACK. `Spectacle::step`
        // advances the effect world, advances the cinematic and — while the cinematic is live —
        // returns the pose `cy::camera::CameraServer::evaluate_stack()` produced. It assigns no
        // camera transform anywhere; what it hands back is what the stack blended out of the two
        // shot rigs the sequence selected. On every other frame it returns `gameplay` unchanged,
        // which is the compiled `cy::graph::camera` rig M8.b built.
        if (Status advanced = advance_spectacle(centroid, view, costs); !advanced) {
            return advanced;
        }

        if (Status framed = presentation_->update_frame(*level_->buffer.readable(), view, report_,
                                                        costs.frame_us);
            !framed) {
            return framed;
        }
        if (Status drawn_shapes = build_shot(); !drawn_shapes) {
            return drawn_shapes;
        }
        if (Status photographed = shoot(static_cast<u32>(tick_)); !photographed) {
            return photographed;
        }
    }

    fold_tick();
    ++tick_;
    return cy::ok();
}

void Slice::fold(u64 value) noexcept {
    digest_ = fold_into(digest_, value);
}

void Slice::fold_tick() noexcept {
    // Every character's placement, quantised. The whole simulation reduces to this plus the module
    // digests folded above, and two runs that agree on it agree on the game.
    for (u32 index = 0; index < characters_.size(); ++index) {
        fold(quantise(characters_.positions[index].x));
        fold(quantise(characters_.positions[index].z));
    }
    fold(tick_);
}

Status Slice::run() noexcept {
    Array<f64> simulation(*allocator_);
    Array<f64> think_cost(*allocator_);
    Array<f64> sense_cost(*allocator_);
    Array<f64> navigate_cost(*allocator_);
    Array<f64> animate_cost(*allocator_);
    Array<f64> abilities_cost(*allocator_);
    Array<f64> per_agent(*allocator_);
    Array<f64> effects(*allocator_);
    Array<f64> interface_cost(*allocator_);
    Array<f64> frame(*allocator_);
    Array<f64> particles(*allocator_);
    Array<f64> cinematic(*allocator_);
    Array<f64> spectacle(*allocator_);
    // The first tenth is warm-up: a cold cache and a first-frame shadow cache are properties of
    // starting rather than of running, and a median over them measures the start.
    const u32 warm_up = options_.ticks / 10U;

    in_loop_ = true;
    const u32 before = compilations_;
    f64 worst = 0.0;
    for (u32 index = 0; index < options_.ticks; ++index) {
        TickCosts costs;
        if (Status stepped = tick(costs); !stepped) {
            in_loop_ = false;
            return stepped;
        }
        if (index < warm_up) {
            continue;
        }
        const f64 total = costs.think_us + costs.sense_us + costs.navigate_us + costs.animate_us +
                          costs.abilities_us + costs.effects_us;
        worst = total > worst ? total : worst;
        if (Status pushed = simulation.push_back(total); !pushed) {
            in_loop_ = false;
            return pushed;
        }
        // ONE TABLE FOR EVERY PER-TICK SAMPLE, and it is one because M8.c's three new phases were
        // first appended as a second loop beside the first and a third block beside four singles:
        // the function measured 27 on this project's cognitive-complexity scale, almost all of it
        // the same four lines of `push_back`-and-check written nine times. Adding a phase is now a
        // row.
        const f64 samples[9] = {
            costs.think_us,
            costs.sense_us,
            costs.navigate_us,
            costs.animate_us,
            costs.abilities_us,
            characters_.size() == 0U ? 0.0 : total / static_cast<f64>(characters_.size()),
            costs.effects_us,
            costs.interface_us,
            costs.frame_us};
        Array<f64>* into[9] = {&think_cost,   &sense_cost,     &navigate_cost,
                               &animate_cost, &abilities_cost, &per_agent,
                               &effects,      &interface_cost, &frame};
        const f64 spectacle_samples[3] = {costs.particles_us, costs.cinematic_us,
                                          costs.spectacle_us()};
        Array<f64>* spectacle_into[3] = {&particles, &cinematic, &spectacle};
        for (u32 phase = 0; phase < 12U; ++phase) {
            Array<f64>& column = phase < 9U ? *into[phase] : *spectacle_into[phase - 9U];
            const f64 value = phase < 9U ? samples[phase] : spectacle_samples[phase - 9U];
            if (Status pushed = column.push_back(value); !pushed) {
                in_loop_ = false;
                return pushed;
            }
        }
        report_.spectacle_us_worst = costs.spectacle_us() > report_.spectacle_us_worst
                                         ? costs.spectacle_us()
                                         : report_.spectacle_us_worst;
    }
    in_loop_ = false;
    compilations_in_loop_ = compilations_ - before;

    report_.simulation_us_median = median_of(simulation);
    report_.simulation_us_worst = worst;
    report_.think_us_median = median_of(think_cost);
    report_.sense_us_median = median_of(sense_cost);
    report_.navigate_us_median = median_of(navigate_cost);
    report_.animate_us_median = median_of(animate_cost);
    report_.abilities_us_median = median_of(abilities_cost);
    report_.per_agent_us_median = median_of(per_agent);
    report_.effects_us_median = median_of(effects);
    report_.interface_us_median = median_of(interface_cost);
    report_.frame_us_median = median_of(frame);
    report_.particles_us_median = median_of(particles);
    report_.cinematic_us_median = median_of(cinematic);
    report_.spectacle_us_median = median_of(spectacle);
    report_.state_digest = digest_;
    return cy::ok();
}

// --- The audit
// --------------------------------------------------------------------------------------

namespace {

/// One per-entity state type of one consumer, and whether it carries a vtable.
struct StateType {
    const char* name;
    bool polymorphic;
};

}  // namespace

Status Slice::audit(InterpretationAudit& out) const noexcept {
    Brain& brain = *brain_;

    // 1. EVERY AUTHORED GRAPH AUDITS CLEAN AND COMPLETE. `AuditReport::complete` is the third
    //    answer beside pass and fail; a check that read `missing` alone would go green on the day a
    //    plugin failed to load and its nodes became opaque.
    const cy::graph::Graph* graphs[5] = {&brain.behaviour_graph, &brain.pose_graph,
                                         &brain.ability_graph, &brain.objective_graph,
                                         &brain.rig_graph};
    for (const cy::graph::Graph* graph : graphs) {
        cy::graph::AuditReport audited;
        cy::graph::DiagnosticSink sink(*allocator_);
        if (Status ran = cy::graph::audit(*graph, brain.registry, audited, sink); !ran) {
            return ran;
        }
        ++out.graphs_audited;
        if (!audited.complete) {
            ++out.graphs_incomplete;
        }
        if (!audited.passed()) {
            ++out.graphs_failed;
        }
    }

    // 2. NOTHING COMPILED INSIDE THE LOOP. A graph consumer that compiled per frame would be an
    //    interpreter with a cache in front of it.
    out.compilations_before_loop = compilations_;
    out.compilations_during_loop = compilations_in_loop_;

    // 3. NO PER-ENTITY STATE TYPE IS POLYMORPHIC. A virtual `tick()` per entity needs a vtable
    //    pointer in the per-entity record, so this is the check stated as what it actually is.
    const StateType types[] = {
        {"ai::AIState", std::is_polymorphic_v<cy::ai::AIState>},
        {"ai::Blackboard", std::is_polymorphic_v<cy::ai::Blackboard>},
        {"ai::AIAgent", std::is_polymorphic_v<cy::ai::AIAgent>},
        {"graph::script::ScriptState", std::is_polymorphic_v<cy::graph::script::ScriptState>},
        {"graph::pose::PoseInstance", std::is_polymorphic_v<cy::graph::pose::PoseInstance>},
        {"animation::AnimationInstance", std::is_polymorphic_v<cy::animation::AnimationInstance>},
        {"navigation::CrowdAgent", std::is_polymorphic_v<cy::navigation::CrowdAgent>},
        {"abilities::AbilityState", std::is_polymorphic_v<cy::gameplay::abilities::AbilityState>},
        {"abilities::EffectInstance",
         std::is_polymorphic_v<cy::gameplay::abilities::EffectInstance>},
        {"graph::camera::RigInstance", std::is_polymorphic_v<cy::graph::camera::RigInstance>},
    };
    for (const StateType& type : types) {
        ++out.state_types_checked;
        if (type.polymorphic) {
            ++out.state_types_polymorphic;
            if (Status pushed = out.offenders.push_back(Name::intern(type.name)); !pushed) {
                return pushed;
            }
        }
    }

    // AND THE CONTROL. `--interpret-control` puts one interpreted object per character in the loop,
    // and this is what has to find it. A `std::is_polymorphic_v` sweep over the ENGINE's types
    // cannot: the offending type is the game's own.
    if (!brain.interpreted.empty()) {
        ++out.state_types_checked;
        ++out.state_types_polymorphic;
        if (Status pushed = out.offenders.push_back(Name::intern("slice::InterpretedNode"));
            !pushed) {
            return pushed;
        }
    }
    return cy::ok();
}

}  // namespace cy::sample::slice
