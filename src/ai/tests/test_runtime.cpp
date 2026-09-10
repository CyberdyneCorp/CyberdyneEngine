// The AI runtime over a real world and a real compiled program: components, the tier policy, the
// deterministic rotation, the budget's deferral, the state hash and the decision history.
// M8.b tasks 6.3 and 6.4.
//
// INTEGRATION: every case here compiles a behaviour graph and builds an ECS world.

#include <cy/ai/locomotion.h>
#include <cy/ai/runtime.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <utility>

#include "ai_fixture.h"

using namespace cy;
using namespace cy::ai;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

[[nodiscard]] ecs::WorldConfig config() noexcept {
    ecs::WorldConfig out;
    out.name = "ai.runtime";
    return out;
}

/// The parallel columns `AiRuntime::think` and `update_tiers` take.
struct Squad {
    Array<Entity> entities;
    Array<AIAgent> agents;
    Array<AIState> states;
    Array<Blackboard> blackboards;
    Array<Vec3> positions;
    Array<KnowledgeStore*> stores;

    explicit Squad(Allocator& alloc) noexcept
        : entities(alloc),
          agents(alloc),
          states(alloc),
          blackboards(alloc),
          positions(alloc),
          stores(alloc) {}

    void add(Vec3 position, u32 slot) noexcept {
        CY_REQUIRE(entities.push_back(Entity::make(static_cast<u32>(entities.size()) + 1u, 1))
                       .has_value());
        AIAgent agent;
        agent.graph = Name::intern("patrol");
        CY_REQUIRE(agents.push_back(agent).has_value());
        AIState state;
        state.slot = slot;
        CY_REQUIRE(states.push_back(state).has_value());
        CY_REQUIRE(blackboards.push_back(Blackboard{}).has_value());
        CY_REQUIRE(positions.push_back(position).has_value());
    }
};

}  // namespace

CY_TEST_CASE("the four components register in a fixed order and a second call binds the same ids") {
    ecs::World world(allocator(), config());
    CY_REQUIRE(world.initialize().has_value());
    const Expected<AiComponents, Error> first = AiComponents::register_all(world);
    CY_REQUIRE(first.has_value());
    CY_CHECK(first->registered());
    const Expected<AiComponents, Error> second = AiComponents::register_all(world);
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(first->agent, second->agent);
    CY_CHECK_LT(first->agent, first->state);
    CY_CHECK_LT(first->state, first->blackboard);
    CY_CHECK_LT(first->blackboard, first->sensors);

    // An agent really is four components on an entity, with no object anywhere.
    const ComponentTypeId components[4] = {first->agent, first->state, first->blackboard,
                                           first->sensors};
    const Expected<Entity, Error> entity = world.create({components, 4});
    CY_REQUIRE(entity.has_value());
    CY_CHECK(world.has(*entity, first->agent));
    // A freshly-created entity's chunk memory is ZERO, so `AIState::slot` must be invalid AT ZERO
    // or a new agent would execute the first agent's program counter, stack and timers. It is.
    CY_CHECK_EQ(world.get<AIState>(*entity, first->state)->slot, kInvalidSlot);
}

CY_TEST_CASE("many agents share one program and differ only in their own state block") {
    // `ai-system`: "WHEN 10,000 agents use one graph THEN ONE compiled program SHALL exist, and
    // per-agent memory SHALL be the state block only."
    graph::NodeRegistry registry(allocator());
    CY_REQUIRE(graph::behaviour::register_behaviour_nodes(registry).has_value());
    graph::DiagnosticSink sink(allocator());
    graph::behaviour::BehaviourProgram program =
        testing::patrol_program(allocator(), registry, sink);
    CY_CHECK_EQ(sink.errors(), 0U);

    AiRuntime runtime(allocator(), AiBudget{}, TierPolicy{});
    const Expected<u32, Error> first = runtime.reserve_slots(program, 64);
    CY_REQUIRE(first.has_value());
    CY_CHECK_EQ(*first, 1U);  // one-based; see kInvalidSlot
    CY_CHECK_EQ(runtime.slot_count(), 64U);

    Squad squad(allocator());
    for (u32 index = 0; index < 64; ++index) {
        squad.add(Vec3{static_cast<f32>(index), 0.0F, 0.0F}, *first + index);
    }

    testing::CountingHost host;
    ThinkReport report;
    CY_REQUIRE(runtime
                   .think(1, program, squad.entities.span(), squad.agents.span(),
                          squad.states.span(), squad.blackboards.span(), squad.stores.span(), host,
                          1.0F / 60.0F, report)
                   .has_value());
    CY_CHECK_EQ(report.agents, 64U);
    CY_CHECK_EQ(report.thought[static_cast<usize>(AiTier::Full)], 64U);
    CY_CHECK_EQ(host.tasks, 64U);
    // One program, sixty-four states.
    CY_CHECK_EQ(runtime.slot_count(), 64U);
    CY_CHECK_EQ(program.digest(), program.digest());
}

CY_TEST_CASE("the rotation spreads a reduced tier across ticks and starves nobody") {
    // `ai-system`: "WHEN 10,000 `Reduced` agents are due to think at 10 Hz THEN they SHALL be
    // distributed across ticks by the rotation rather than all thinking on the same tick", and "the
    // rotation SHALL guarantee that every agent thinks within a bounded number of ticks".
    graph::NodeRegistry registry(allocator());
    CY_REQUIRE(graph::behaviour::register_behaviour_nodes(registry).has_value());
    graph::DiagnosticSink sink(allocator());
    graph::behaviour::BehaviourProgram program =
        testing::patrol_program(allocator(), registry, sink);

    AiRuntime runtime(allocator(), AiBudget{}, TierPolicy{});
    const Expected<u32, Error> first = runtime.reserve_slots(program, 120);
    CY_REQUIRE(first.has_value());
    Squad squad(allocator());
    for (u32 index = 0; index < 120; ++index) {
        squad.add(Vec3{}, *first + index);
        squad.agents[index].tier = AiTier::Reduced;
    }

    testing::CountingHost host;
    const u32 interval = tier_think_interval(AiTier::Reduced);
    u32 thought_total = 0;
    u32 starved_total = 0;
    for (u32 tick = 0; tick < interval * 3; ++tick) {
        ThinkReport report;
        CY_REQUIRE(runtime
                       .think(tick, program, squad.entities.span(), squad.agents.span(),
                              squad.states.span(), squad.blackboards.span(), squad.stores.span(),
                              host, 1.0F / 60.0F, report)
                       .has_value());
        // A twentieth of the squad each tick, never all of it.
        CY_REQUIRE_EQ(report.thought[static_cast<usize>(AiTier::Reduced)], 120U / interval);
        thought_total += report.thought[static_cast<usize>(AiTier::Reduced)];
        starved_total += report.starved;
    }
    CY_CHECK_EQ(thought_total, 360U);
    CY_CHECK_EQ(starved_total, 0U);
    // And every single agent had its turn.
    for (const AIState& state : squad.states.span()) {
        CY_REQUIRE(state.last_think_tick > 0U);
    }
}

CY_TEST_CASE("the budget defers within a tick and the deferred agent keeps its place") {
    graph::NodeRegistry registry(allocator());
    CY_REQUIRE(graph::behaviour::register_behaviour_nodes(registry).has_value());
    graph::DiagnosticSink sink(allocator());
    graph::behaviour::BehaviourProgram program =
        testing::patrol_program(allocator(), registry, sink);

    AiBudget budget;
    budget.thinks_per_tick[static_cast<usize>(AiTier::Full)] = 4;
    AiRuntime runtime(allocator(), budget, TierPolicy{});
    const Expected<u32, Error> first = runtime.reserve_slots(program, 10);
    CY_REQUIRE(first.has_value());
    Squad squad(allocator());
    for (u32 index = 0; index < 10; ++index) {
        squad.add(Vec3{}, *first + index);
    }

    testing::CountingHost host;
    ThinkReport report;
    CY_REQUIRE(runtime
                   .think(1, program, squad.entities.span(), squad.agents.span(),
                          squad.states.span(), squad.blackboards.span(), squad.stores.span(), host,
                          1.0F / 60.0F, report)
                   .has_value());
    CY_CHECK(report.budget_exceeded);
    CY_CHECK_EQ(report.thought[static_cast<usize>(AiTier::Full)], 4U);
    CY_CHECK_EQ(report.deferred, 6U);
    // Deferred, not dropped: the untouched agents still have no think tick, so the next tick's
    // rotation offers them again and the starvation counter can see them.
    CY_CHECK_EQ(squad.states[9].last_think_tick, 0U);
}

CY_TEST_CASE("Adaptive mode is refused with lockstep, at configuration time") {
    // `ai-system`: "WHEN a project selects `Adaptive` and then enables lockstep networking THEN the
    // engine SHALL report the incompatibility AT CONFIGURATION TIME, not at desync time."
    AiBudget deterministic;
    AiRuntime honest(allocator(), deterministic, TierPolicy{});
    CY_CHECK(honest.check_lockstep(true).has_value());

    AiBudget adaptive;
    adaptive.mode = BudgetMode::Adaptive;
    AiRuntime fast(allocator(), adaptive, TierPolicy{});
    CY_CHECK(fast.check_lockstep(false).has_value());
    const Status refused = fast.check_lockstep(true);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::InvalidArgument);
}

CY_TEST_CASE("tiers follow distance, hold through hysteresis, and a pin is never demoted") {
    TierPolicy policy;
    policy.full_distance = 30.0F;
    policy.reduced_distance = 80.0F;
    policy.minimal_distance = 200.0F;
    policy.hysteresis = 8.0F;
    AiRuntime runtime(allocator(), AiBudget{}, policy);

    Array<AIAgent> agents(allocator());
    Array<Vec3> positions(allocator());
    Array<AIState> states(allocator());
    const f32 distances[4] = {10.0F, 50.0F, 150.0F, 500.0F};
    for (const f32 distance : distances) {
        AIAgent agent;
        CY_REQUIRE(agents.push_back(agent).has_value());
        CY_REQUIRE(positions.push_back(Vec3{distance, 0.0F, 0.0F}).has_value());
        CY_REQUIRE(states.push_back(AIState{}).has_value());
    }
    const Vec3 observer{};

    TierReport report;
    CY_REQUIRE(
        runtime.update_tiers(agents.span(), positions.span(), states.span(), {&observer, 1}, report)
            .has_value());
    CY_CHECK_EQ(agents[0].tier, AiTier::Full);
    CY_CHECK_EQ(agents[1].tier, AiTier::Reduced);
    CY_CHECK_EQ(agents[2].tier, AiTier::Minimal);
    CY_CHECK_EQ(agents[3].tier, AiTier::Statistical);

    // HYSTERESIS: an agent that walks just past the threshold keeps what it had.
    positions[0] = Vec3{34.0F, 0.0F, 0.0F};
    CY_REQUIRE(
        runtime.update_tiers(agents.span(), positions.span(), states.span(), {&observer, 1}, report)
            .has_value());
    CY_CHECK_EQ(agents[0].tier, AiTier::Full);
    // Past the margin it gives way.
    positions[0] = Vec3{45.0F, 0.0F, 0.0F};
    CY_REQUIRE(
        runtime.update_tiers(agents.span(), positions.span(), states.span(), {&observer, 1}, report)
            .has_value());
    CY_CHECK_EQ(agents[0].tier, AiTier::Reduced);
    CY_CHECK_EQ(report.demoted, 1U);

    // A PINNED agent is never demoted, however far away.
    agents[3].pinned = AiTier::Full;
    CY_REQUIRE(
        runtime.update_tiers(agents.span(), positions.span(), states.span(), {&observer, 1}, report)
            .has_value());
    CY_CHECK_EQ(agents[3].tier, AiTier::Full);
    CY_CHECK_GT(report.pinned_held, 0U);
    // And so is a gameplay-critical one.
    agents[2].critical = true;
    CY_REQUIRE(
        runtime.update_tiers(agents.span(), positions.span(), states.span(), {&observer, 1}, report)
            .has_value());
    CY_CHECK_EQ(agents[2].tier, AiTier::Full);
}

CY_TEST_CASE("promotion out of a coarse tier reconstructs state rather than resuming a stale one") {
    // `ai-system`: "WHEN a `Statistical` agent is promoted as an observer approaches THEN
    // individual state SHALL be reconstructed ... without a visible behavioural discontinuity."
    graph::NodeRegistry registry(allocator());
    CY_REQUIRE(graph::behaviour::register_behaviour_nodes(registry).has_value());
    graph::DiagnosticSink sink(allocator());
    graph::behaviour::BehaviourProgram program =
        testing::patrol_program(allocator(), registry, sink);

    AiRuntime runtime(allocator(), AiBudget{}, TierPolicy{});
    const Expected<u32, Error> slot = runtime.reserve_slots(program, 1);
    CY_REQUIRE(slot.has_value());

    Array<AIAgent> agents(allocator());
    Array<Vec3> positions(allocator());
    Array<AIState> states(allocator());
    AIAgent agent;
    agent.tier = AiTier::Statistical;
    CY_REQUIRE(agents.push_back(agent).has_value());
    CY_REQUIRE(positions.push_back(Vec3{5.0F, 0.0F, 0.0F}).has_value());
    AIState state;
    state.slot = *slot;
    CY_REQUIRE(states.push_back(state).has_value());

    // Leave a stale program counter behind, as a long-dormant agent would have.
    Squad squad(allocator());
    squad.add(Vec3{}, *slot);
    testing::CountingHost host;
    host.task_result = graph::behaviour::BtStatus::Running;
    ThinkReport ticked;
    CY_REQUIRE(runtime
                   .think(1, program, squad.entities.span(), squad.agents.span(),
                          squad.states.span(), squad.blackboards.span(), squad.stores.span(), host,
                          0.1F, ticked)
                   .has_value());
    CY_REQUIRE_NE(runtime.state(*slot)->running(), 0xFFFFU);

    const Vec3 observer{};
    TierReport report;
    CY_REQUIRE(
        runtime.update_tiers(agents.span(), positions.span(), states.span(), {&observer, 1}, report)
            .has_value());
    CY_CHECK_EQ(agents[0].tier, AiTier::Full);
    CY_CHECK_EQ(report.promoted, 1U);
    CY_CHECK_EQ(report.reconstructed, 1U);
    // The stale program counter is gone: the agent starts from the root rather than resuming a
    // decision it made a thousand ticks ago.
    CY_CHECK_EQ(runtime.state(*slot)->running(), 0xFFFFU);
}

CY_TEST_CASE("the same tick twice produces the same state hash, and a divergence changes it") {
    // `ai-system`: "A determinism test mode SHALL hash AI state per tick so divergence is
    // detectable and localisable."
    graph::NodeRegistry registry(allocator());
    CY_REQUIRE(graph::behaviour::register_behaviour_nodes(registry).has_value());
    graph::DiagnosticSink sink(allocator());
    graph::behaviour::BehaviourProgram program =
        testing::patrol_program(allocator(), registry, sink);

    const auto run = [&program](bool condition) noexcept {
        AiRuntime runtime(allocator(), AiBudget{}, TierPolicy{});
        const Expected<u32, Error> first = runtime.reserve_slots(program, 8);
        CY_REQUIRE(first.has_value());
        Squad squad(allocator());
        for (u32 index = 0; index < 8; ++index) {
            squad.add(Vec3{}, *first + index);
        }
        testing::CountingHost host;
        host.condition_result = condition;
        host.task_result = graph::behaviour::BtStatus::Running;
        u64 hash = 0;
        for (u32 tick = 0; tick < 4; ++tick) {
            ThinkReport report;
            CY_REQUIRE(runtime
                           .think(tick, program, squad.entities.span(), squad.agents.span(),
                                  squad.states.span(), squad.blackboards.span(),
                                  squad.stores.span(), host, 0.1F, report)
                           .has_value());
            hash = report.state_hash;
        }
        return hash;
    };
    CY_CHECK_EQ(run(true), run(true));
    CY_CHECK_NE(run(true), run(false));
}

CY_TEST_CASE("the decision history is rolling, bounded, and answers why one agent decided") {
    graph::NodeRegistry registry(allocator());
    CY_REQUIRE(graph::behaviour::register_behaviour_nodes(registry).has_value());
    graph::DiagnosticSink sink(allocator());
    graph::behaviour::BehaviourProgram program =
        testing::patrol_program(allocator(), registry, sink);

    AiRuntime runtime(allocator(), AiBudget{}, TierPolicy{});
    CY_REQUIRE(runtime.set_history_capacity(8).has_value());
    const Expected<u32, Error> first = runtime.reserve_slots(program, 2);
    CY_REQUIRE(first.has_value());
    Squad squad(allocator());
    squad.add(Vec3{}, *first);
    squad.add(Vec3{}, *first + 1);

    testing::CountingHost host;
    for (u32 tick = 0; tick < 10; ++tick) {
        ThinkReport report;
        CY_REQUIRE(runtime
                       .think(tick, program, squad.entities.span(), squad.agents.span(),
                              squad.states.span(), squad.blackboards.span(), squad.stores.span(),
                              host, 0.1F, report)
                       .has_value());
    }
    // Twenty decisions were made and eight are kept: rolling, and bounded.
    CY_CHECK_EQ(runtime.history().size(), usize{8});

    Array<DecisionRecord> mine(allocator());
    CY_REQUIRE(runtime.history_of(squad.entities[0], mine).has_value());
    CY_CHECK_EQ(mine.size(), usize{4});
    for (usize index = 1; index < mine.size(); ++index) {
        // Oldest first, which is what "inspectable after the fact" needs to mean something.
        CY_REQUIRE(mine[index].tick > mine[index - 1].tick);
    }
    CY_CHECK_EQ(mine[0].tier, AiTier::Full);

    // Off by default: a runtime that recorded everything for eight thousand agents would be a leak
    // with a debugger attached.
    AiRuntime quiet(allocator(), AiBudget{}, TierPolicy{});
    CY_CHECK(quiet.history().empty());
}

CY_TEST_CASE("an agent whose slot was never reserved is refused with a diagnostic") {
    graph::NodeRegistry registry(allocator());
    CY_REQUIRE(graph::behaviour::register_behaviour_nodes(registry).has_value());
    graph::DiagnosticSink sink(allocator());
    graph::behaviour::BehaviourProgram program =
        testing::patrol_program(allocator(), registry, sink);

    AiRuntime runtime(allocator(), AiBudget{}, TierPolicy{});
    Squad squad(allocator());
    squad.add(Vec3{}, kInvalidSlot);
    testing::CountingHost host;
    ThinkReport report;
    const Status refused =
        runtime.think(1, program, squad.entities.span(), squad.agents.span(), squad.states.span(),
                      squad.blackboards.span(), squad.stores.span(), host, 0.1F, report);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::InvalidArgument);
}

CY_TEST_CASE("a tier decides the movement strategy, and nothing else in AI moves an agent") {
    // `ai-system`: "WHEN an agent is demoted to `Reduced` THEN it SHALL follow a shared flow field
    // rather than computing an individual path."
    Array<AIAgent> agents(allocator());
    Array<navigation::NavAgent> movers(allocator());
    const AiTier tiers[4] = {AiTier::Full, AiTier::Reduced, AiTier::Minimal, AiTier::Statistical};
    for (const AiTier tier : tiers) {
        AIAgent agent;
        agent.tier = tier;
        CY_REQUIRE(agents.push_back(agent).has_value());
        CY_REQUIRE(movers.push_back(navigation::NavAgent{}).has_value());
    }

    LocomotionReport report;
    CY_REQUIRE(apply_tier_to_movement(agents.span(), movers.span(), report).has_value());
    CY_CHECK_EQ(report.agents, 4U);
    CY_CHECK_EQ(report.pathing_individually, 1U);
    CY_CHECK_EQ(report.following_field, 3U);
    CY_CHECK_FALSE(movers[0].follows_field);
    CY_CHECK(movers[1].follows_field);
    CY_CHECK(movers[3].follows_field);
    CY_CHECK_EQ(report.changed, 3U);

    // Idempotent: a second pass changes nothing.
    CY_REQUIRE(apply_tier_to_movement(agents.span(), movers.span(), report).has_value());
    CY_CHECK_EQ(report.changed, 0U);

    // A promotion puts the agent back on its own path, and clears whatever it was doing so a
    // completing field-follow is not applied to an agent that is now pathing.
    agents[1].tier = AiTier::Full;
    movers[1].status = navigation::NavPathStatus::Following;
    CY_REQUIRE(apply_tier_to_movement(agents.span(), movers.span(), report).has_value());
    CY_CHECK_FALSE(movers[1].follows_field);
    CY_CHECK_EQ(movers[1].status, navigation::NavPathStatus::Idle);

    // Mismatched columns are refused rather than read past their end.
    Status refused = apply_tier_to_movement(agents.span(), Span<navigation::NavAgent>{}, report);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::InvalidArgument);
}

CY_TEST_CASE("a GOAP plan runs through the runtime with a budget it reports") {
    // `ai-system`: "goals with a desired world state, actions with preconditions, effects, and
    // costs, and a planner producing a valid action sequence", with "a maximum search budget" and
    // "the same world state and action set SHALL produce the same plan".
    graph::NodeRegistry registry(allocator());
    CY_REQUIRE(graph::behaviour::register_behaviour_nodes(registry).has_value());
    graph::Graph plan(allocator(), Name::intern("forage"));
    CY_REQUIRE(plan.add_node(1, Name::intern("ai.root")).has_value());
    CY_REQUIRE(plan.add_node(2, Name::intern("ai.plan")).has_value());
    CY_REQUIRE(plan.set_property(2, Name::intern("goal"), testing::bits(0x4U)).has_value());
    CY_REQUIRE(plan.connect(2, Name::intern("node"), 1, Name::intern("child")).has_value());
    // Two operators: gather, then deliver. Only the pair reaches the goal.
    CY_REQUIRE(plan.add_node(3, Name::intern("ai.operator")).has_value());
    CY_REQUIRE(plan.set_property(3, Name::intern("task"), testing::text("gather")).has_value());
    CY_REQUIRE(plan.set_property(3, Name::intern("sets"), testing::bits(0x1U)).has_value());
    CY_REQUIRE(plan.add_node(4, Name::intern("ai.operator")).has_value());
    CY_REQUIRE(plan.set_property(4, Name::intern("task"), testing::text("deliver")).has_value());
    CY_REQUIRE(plan.set_property(4, Name::intern("requires"), testing::bits(0x1U)).has_value());
    CY_REQUIRE(plan.set_property(4, Name::intern("sets"), testing::bits(0x4U)).has_value());
    plan.resolve(registry);

    graph::DiagnosticSink sink(allocator());
    Expected<graph::behaviour::BehaviourProgram, Error> program =
        graph::behaviour::compile_behaviour(plan, registry, sink);
    CY_REQUIRE(program.has_value());
    CY_CHECK_EQ(sink.errors(), 0U);
    CY_CHECK_EQ(program->operators().size(), usize{2});

    AiRuntime runtime(allocator(), AiBudget{}, TierPolicy{});
    const Expected<u32, Error> slot = runtime.reserve_slots(*program, 1);
    CY_REQUIRE(slot.has_value());
    Squad squad(allocator());
    squad.add(Vec3{}, *slot);

    testing::CountingHost host;
    host.task_result = graph::behaviour::BtStatus::Success;
    ThinkReport report;
    CY_REQUIRE(runtime
                   .think(1, *program, squad.entities.span(), squad.agents.span(),
                          squad.states.span(), squad.blackboards.span(), squad.stores.span(), host,
                          0.1F, report)
                   .has_value());
    // The search ran and was measured, and the first step of the plan was executed.
    CY_CHECK_GT(report.plan_nodes, 0U);
    CY_CHECK_GT(host.tasks, 0U);
    CY_CHECK_EQ(runtime.state(*slot)->plan().size(), usize{2});
    CY_CHECK_EQ(runtime.state(*slot)->last_plan_nodes(), report.plan_nodes);

    // Deterministic: the same world state and operator table produce the same plan.
    AgentState fresh(allocator(), *program);
    fresh.set_world(0);
    const u32 first_search = graph::behaviour::plan_towards(*program, fresh, 0x4U, 64);
    AgentState again(allocator(), *program);
    again.set_world(0);
    const u32 second_search = graph::behaviour::plan_towards(*program, again, 0x4U, 64);
    CY_CHECK_EQ(first_search, second_search);
    CY_REQUIRE_EQ(fresh.plan().size(), again.plan().size());
    for (usize index = 0; index < fresh.plan().size(); ++index) {
        CY_REQUIRE_EQ(fresh.plan()[index], again.plan()[index]);
    }

    // A budget too small to reach the goal leaves no plan and reports what it spent, rather than
    // stalling. `ai-system`: "it SHALL report failure deterministically".
    AgentState starved(allocator(), *program);
    starved.set_world(0);
    const u32 spent = graph::behaviour::plan_towards(*program, starved, 0x4U, 1);
    CY_CHECK_LE(spent, 1U);
    CY_CHECK(starved.plan().empty());
}
