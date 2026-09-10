// The milestone's exit criterion for AI, measured: eight thousand agents thinking, sensing and
// planning inside a declared budget, and the same world torn down under load. M8.b section 6.
//
// `docs/ROADMAP.md`, M8.b exit criteria: "Cost is bounded by configuration: 8,000 agents and 100
// concurrent effects hold their budgets."
//
// WHAT IS REPORTED AND WHY IT IS NOT AN EXTREME. The headline is the MEDIAN tick and a per-agent
// figure derived from it, with the worst tick beside it — not the best. A number picked from the
// fastest tick of a run measures the machine's cache, not the code. The threshold is set at several
// times the budget so this is a REGRESSION DETECTOR rather than a benchmark of this host: it fails
// on a change that makes the schedule an order of magnitude more expensive, and passes on a slower
// machine that is running the same code.

#include <cy/ai/perception.h>
#include <cy/ai/runtime.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <ctime>

#include <chrono>
#include <utility>

#include "ai_fixture.h"

using namespace cy;
using namespace cy::ai;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

constexpr u32 kAgents = 8000;
constexpr u32 kTicks = 120;

class SilentHost final : public graph::behaviour::BehaviourHost {
public:
    graph::behaviour::BtStatus run_task(Name /*task*/, f32 /*dt*/) override {
        ++tasks;
        return graph::behaviour::BtStatus::Running;
    }
    [[nodiscard]] bool test_condition(Name /*condition*/) override {
        ++conditions;
        return true;
    }
    [[nodiscard]] f32 score(Name /*task*/) override { return 0.5F; }

    u64 tasks = 0;
    u64 conditions = 0;
};

class NullVisibility final : public VisibilityHost {
public:
    void trace_batch(Span<const VisibilityQuery> queries, Span<bool> results) override {
        ++calls;
        traced += static_cast<u64>(queries.size());
        for (bool& result : results) {
            result = true;
        }
    }

    u64 calls = 0;
    u64 traced = 0;
};

/// Eight thousand agents' worth of parallel columns.
struct Army {
    Array<Entity> entities;
    Array<AIAgent> agents;
    Array<AIState> states;
    Array<Blackboard> blackboards;
    Array<Vec3> positions;
    Array<Vec3> forward;
    Array<PerceptionSensors> sensors;
    Array<f32> importance;
    Array<AiTier> tiers;
    Array<KnowledgeStore> knowledge;
    Array<KnowledgeStore*> stores;

    explicit Army(Allocator& alloc) noexcept
        : entities(alloc),
          agents(alloc),
          states(alloc),
          blackboards(alloc),
          positions(alloc),
          forward(alloc),
          sensors(alloc),
          importance(alloc),
          tiers(alloc),
          knowledge(alloc),
          stores(alloc) {}
};

void muster(Army& army, AiRuntime& runtime,
            const graph::behaviour::BehaviourProgram& program) noexcept {
    const Expected<u32, Error> first = runtime.reserve_slots(program, kAgents);
    CY_REQUIRE(first.has_value());
    KnowledgeParams knowledge_params;
    knowledge_params.capacity = 8;
    CY_REQUIRE(army.knowledge.reserve(kAgents).has_value());

    for (u32 index = 0; index < kAgents; ++index) {
        CY_REQUIRE(army.entities.push_back(Entity::make(index + 1u, 1)).has_value());
        AIAgent agent;
        agent.graph = Name::intern("patrol");
        agent.importance = 1.0F + static_cast<f32>(index % 3u);
        CY_REQUIRE(army.agents.push_back(agent).has_value());
        AIState state;
        state.slot = *first + index;
        CY_REQUIRE(army.states.push_back(state).has_value());
        CY_REQUIRE(army.blackboards.push_back(Blackboard{}).has_value());
        // A hundred metres square: near the observer at the origin some are `Full`, most are not.
        const u32 row_index = index / 100u;
        const f32 column = static_cast<f32>(index % 100u) * 2.0F;
        const f32 row = static_cast<f32>(row_index) * 2.0F;
        CY_REQUIRE(army.positions.push_back(Vec3{column, 0.0F, row}).has_value());
        CY_REQUIRE(army.forward.push_back(Vec3{1.0F, 0.0F, 0.0F}).has_value());
        PerceptionSensors sensor;
        sensor.own_faction = 1;
        CY_REQUIRE(army.sensors.push_back(sensor).has_value());
        CY_REQUIRE(army.importance.push_back(agent.importance).has_value());
        CY_REQUIRE(army.tiers.push_back(AiTier::Full).has_value());
        CY_REQUIRE(
            army.knowledge.push_back(KnowledgeStore(allocator(), knowledge_params)).has_value());
    }
    for (u32 index = 0; index < kAgents; ++index) {
        CY_REQUIRE(army.stores.push_back(&army.knowledge[index]).has_value());
    }
}

/// This thread's CPU time, in milliseconds.
///
/// CPU TIME AND NOT WALL CLOCK, and that is the whole reason this helper exists. A budget asserted
/// on elapsed wall time measures whatever else the machine is running: on a host in the middle of a
/// fourteen-job build, one tick of this loop was descheduled long enough to fail a threshold with
/// six times the headroom, and a test that fails when the machine is busy is a test that gets
/// switched off. The test harness's own budget guard makes the same choice for the same reason —
/// see tests/harness/src/budget.cpp, whose `cpu_now_ns` this mirrors.
///
/// Per-thread rather than per-process: nothing here fans work across threads, and a process clock
/// would count every worker some other part of the process happened to start.
[[nodiscard]] f64 cpu_millis() noexcept {
#if defined(CLOCK_THREAD_CPUTIME_ID)
    timespec now{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0) {
        return (static_cast<f64>(now.tv_sec) * 1000.0) + (static_cast<f64>(now.tv_nsec) / 1e6);
    }
#endif
    const auto since = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<f64, std::milli>(since).count();
}

[[nodiscard]] f64 median_of(Array<f64>& samples) noexcept {
    for (usize i = 0; i < samples.size(); ++i) {
        for (usize j = i + 1; j < samples.size(); ++j) {
            if (samples[j] < samples[i]) {
                const f64 swap = samples[i];
                samples[i] = samples[j];
                samples[j] = swap;
            }
        }
    }
    return samples[samples.size() / 2];
}

}  // namespace

CY_TEST_CASE("eight thousand agents think, sense and hold their declared budget") {
    graph::NodeRegistry registry(allocator());
    CY_REQUIRE(graph::behaviour::register_behaviour_nodes(registry).has_value());
    graph::DiagnosticSink sink(allocator());
    graph::behaviour::BehaviourProgram program =
        testing::patrol_program(allocator(), registry, sink);
    CY_REQUIRE_EQ(sink.errors(), 0U);

    AiBudget budget;
    budget.thinks_per_tick[static_cast<usize>(AiTier::Full)] = kAgents;
    budget.thinks_per_tick[static_cast<usize>(AiTier::Reduced)] = kAgents;
    AiRuntime runtime(allocator(), budget, TierPolicy{});
    Army army(allocator());
    muster(army, runtime, program);

    PerceptionParams perception_params;
    perception_params.query_budget = 512;
    PerceptionScheduler perception(allocator(), perception_params);

    Array<PerceptionTarget> targets(allocator());
    for (u32 index = 0; index < 16; ++index) {
        PerceptionTarget target;
        target.entity = Entity::make(100000u + index, 1);
        target.position = Vec3{static_cast<f32>(index) * 12.0F, 0.0F, 40.0F};
        target.faction = 2;
        target.relevance = 1.0F;
        CY_REQUIRE(targets.push_back(target).has_value());
    }
    const Vec3 observer{100.0F, 0.0F, 80.0F};

    SilentHost host;
    NullVisibility eyes;
    Array<f64> think_ms(allocator());
    Array<f64> sense_ms(allocator());
    Array<f64> instructions(allocator());
    ThinkReport last_think;
    TierReport last_tiers;
    PerceptionReport last_perception;
    u32 starved_total = 0;

    for (u32 tick = 0; tick < kTicks; ++tick) {
        CY_REQUIRE(runtime
                       .update_tiers(army.agents.span(), army.positions.span(), army.states.span(),
                                     {&observer, 1}, last_tiers)
                       .has_value());
        for (u32 index = 0; index < kAgents; ++index) {
            army.tiers[index] = army.agents[index].tier;
        }

        const f64 sense_started = cpu_millis();
        const ObserverColumns columns{
            army.entities.span(),   army.positions.span(), army.forward.span(), army.sensors.span(),
            army.importance.span(), army.tiers.span(),     army.stores.span()};
        CY_REQUIRE(
            perception.update(tick, columns, targets.span(), eyes, last_perception).has_value());
        const f64 sense_finished = cpu_millis();

        const f64 think_started = cpu_millis();
        CY_REQUIRE(runtime
                       .think(tick, program, army.entities.span(), army.agents.span(),
                              army.states.span(), army.blackboards.span(), army.stores.span(), host,
                              1.0F / 60.0F, last_think)
                       .has_value());
        const f64 think_finished = cpu_millis();

        starved_total += last_think.starved;
        CY_REQUIRE(instructions.push_back(static_cast<f64>(last_think.instructions)).has_value());
        CY_REQUIRE(
            think_ms
                .push_back(
                    std::chrono::duration<f64, std::milli>(think_finished - think_started).count())
                .has_value());
        CY_REQUIRE(
            sense_ms
                .push_back(
                    std::chrono::duration<f64, std::milli>(sense_finished - sense_started).count())
                .has_value());
    }

    const f64 work_median = median_of(instructions);
    const f64 work_worst = instructions[instructions.size() - 1];
    const f64 think_median = median_of(think_ms);
    const f64 sense_median = median_of(sense_ms);
    const f64 think_worst = think_ms[think_ms.size() - 1];
    const f64 sense_worst = sense_ms[sense_ms.size() - 1];
    const f64 per_agent_us = ((think_median + sense_median) * 1000.0) / static_cast<f64>(kAgents);

    CY_TEST_MESSAGE("8000 agents, 120 ticks of CPU time: think median "
                    << think_median << " ms (worst " << think_worst << "), sense median "
                    << sense_median << " ms (worst " << sense_worst << "), " << per_agent_us
                    << " us/agent/tick, starved " << starved_total);
    CY_TEST_MESSAGE("last tick: Full "
                    << last_tiers.by_tier[0] << ", Reduced " << last_tiers.by_tier[1]
                    << ", Minimal " << last_tiers.by_tier[2] << ", Statistical "
                    << last_tiers.by_tier[3] << "; thought "
                    << last_think.thought[0] + last_think.thought[1] + last_think.thought[2]
                    << ", tasks run " << host.tasks << ", traces " << eyes.traced);

    // The work really happened: a measurement of a loop that did nothing would also be fast.
    CY_CHECK_GT(last_think.thought[static_cast<usize>(AiTier::Full)], 0U);
    CY_CHECK_GT(host.tasks, u64{0});
    CY_CHECK_GT(eyes.traced, u64{0});

    // THE REQUIREMENTS, not the timings.
    CY_CHECK_EQ(last_think.agents, kAgents);
    // Nobody was starved: the rotation's guarantee, over a hundred and twenty ticks.
    CY_CHECK_EQ(starved_total, 0U);
    // Tiers really did spread: an observer in the corner of a hundred-metre field means most agents
    // are not `Full`, which is the whole of what AI LOD buys.
    CY_CHECK_GT(last_tiers.by_tier[static_cast<usize>(AiTier::Full)], 0U);
    CY_CHECK_GT(last_tiers.by_tier[static_cast<usize>(AiTier::Reduced)] +
                    last_tiers.by_tier[static_cast<usize>(AiTier::Minimal)] +
                    last_tiers.by_tier[static_cast<usize>(AiTier::Statistical)],
                last_tiers.by_tier[static_cast<usize>(AiTier::Full)]);
    // Perception issued a BOUNDED set of queries, in one call, for eight thousand agents.
    CY_CHECK_LE(last_perception.queries_issued, perception_params.query_budget);
    CY_CHECK_LE(eyes.calls, u64{kTicks});

    // THE BUDGET. Eight thousand agents at 60 Hz have 16.6 ms of frame; AI's declared share is a
    // quarter of it, so ~2 us per agent per tick before LOD. The threshold is four times that, for
    // the reason the header gives.
    CY_CHECK_LT(per_agent_us, 8.0);
    // AND THE WORK ITSELF IS STABLE TICK TO TICK. The structural form of "the schedule did not
    // degrade": instructions evaluated is a pure function of the tick, the tiers and the programs,
    // so unlike an elapsed-time ratio it says nothing about what else this machine was running. A
    // rotation that collapsed and made every agent think every tick would show here.
    CY_CHECK_LT(work_worst, work_median * 3.0);
    // The worst tick is REPORTED and not asserted. Even on the CPU clock it carries the cost of a
    // cold cache and of whatever the scheduler did on the first tick, and turning that into a
    // threshold would be measuring the host rather than the code. `work_worst` above is the
    // regression detector it would have been standing in for.
}

CY_TEST_CASE("a runtime torn down mid-think releases every agent's state") {
    // Hard rule 4 of this milestone's brief: teardown under load. Twelve runtimes, each with two
    // thousand agents, each ticked and then destroyed with agents still running — under a sanitizer
    // this is what finds a leaked execution state or a dangling knowledge store.
    graph::NodeRegistry registry(allocator());
    CY_REQUIRE(graph::behaviour::register_behaviour_nodes(registry).has_value());
    graph::DiagnosticSink sink(allocator());
    graph::behaviour::BehaviourProgram program =
        testing::patrol_program(allocator(), registry, sink);

    for (u32 round = 0; round < 12; ++round) {
        AiRuntime runtime(allocator(), AiBudget{}, TierPolicy{});
        CY_REQUIRE(runtime.set_history_capacity(256).has_value());
        const Expected<u32, Error> first = runtime.reserve_slots(program, 2000);
        CY_REQUIRE(first.has_value());

        Array<Entity> entities(allocator());
        Array<AIAgent> agents(allocator());
        Array<AIState> states(allocator());
        Array<Blackboard> blackboards(allocator());
        Array<KnowledgeStore*> stores(allocator());
        for (u32 index = 0; index < 2000; ++index) {
            CY_REQUIRE(entities.push_back(Entity::make(index + 1u, 1)).has_value());
            CY_REQUIRE(agents.push_back(AIAgent{}).has_value());
            AIState state;
            state.slot = *first + index;
            CY_REQUIRE(states.push_back(state).has_value());
            CY_REQUIRE(blackboards.push_back(Blackboard{}).has_value());
        }

        SilentHost host;
        ThinkReport report;
        CY_REQUIRE(runtime
                       .think(round, program, entities.span(), agents.span(), states.span(),
                              blackboards.span(), stores.span(), host, 1.0F / 60.0F, report)
                       .has_value());
        CY_CHECK_EQ(report.agents, 2000U);
        // Destroyed here, with two thousand agents mid-`Running` and a history that has wrapped.
    }
}
