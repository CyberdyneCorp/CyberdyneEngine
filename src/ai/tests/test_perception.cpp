// Batched perception: the rotation, the broad phase, sharing, the budget and its deferral, and the
// direct path. M8.b task 6.4.
//
// INTEGRATION: a case here runs a thousand observers against a hundred targets to show that the
// query count is bounded rather than per agent, which is the requirement and is over the unit
// tier's millisecond by construction.

#include <cy/ai/perception.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::ai;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// A host that answers every trace, and counts how many it was handed and in how many calls.
class CountingHost final : public VisibilityHost {
public:
    void trace_batch(Span<const VisibilityQuery> queries, Span<bool> results) override {
        ++calls;
        traced += static_cast<u32>(queries.size());
        for (bool& result : results) {
            result = clear;
        }
    }

    u32 calls = 0;
    u32 traced = 0;
    bool clear = true;
};

/// The parallel columns `PerceptionScheduler::update` takes, built once so a case says only what it
/// changes.
struct Observers {
    Array<Entity> entities;
    Array<Vec3> positions;
    Array<Vec3> forward;
    Array<PerceptionSensors> sensors;
    Array<f32> importance;
    Array<AiTier> tiers;
    Array<KnowledgeStore*> stores;
    Array<KnowledgeStore> owned;

    [[nodiscard]] ObserverColumns columns() noexcept {
        return ObserverColumns{entities.span(),   positions.span(), forward.span(), sensors.span(),
                               importance.span(), tiers.span(),     stores.span()};
    }

    explicit Observers(Allocator& alloc) noexcept
        : entities(alloc),
          positions(alloc),
          forward(alloc),
          sensors(alloc),
          importance(alloc),
          tiers(alloc),
          stores(alloc),
          owned(alloc) {}

    void add(Vec3 position, AiTier tier, f32 weight) noexcept {
        CY_REQUIRE(entities.push_back(Entity::make(static_cast<u32>(entities.size()) + 1u, 1))
                       .has_value());
        CY_REQUIRE(positions.push_back(position).has_value());
        CY_REQUIRE(forward.push_back(Vec3{1.0F, 0.0F, 0.0F}).has_value());
        PerceptionSensors sensor;
        sensor.own_faction = 1;
        sensor.factions_of_interest = ~u64{0};
        CY_REQUIRE(sensors.push_back(sensor).has_value());
        CY_REQUIRE(importance.push_back(weight).has_value());
        CY_REQUIRE(tiers.push_back(tier).has_value());
    }

    /// Called once, after every `add`: the stores are heap-stable only while `owned` does not grow.
    void bind() noexcept {
        KnowledgeParams params;
        params.capacity = 8;
        CY_REQUIRE(owned.reserve(entities.size()).has_value());
        for (usize index = 0; index < entities.size(); ++index) {
            CY_REQUIRE(owned.push_back(KnowledgeStore(owned.allocator(), params)).has_value());
        }
        for (usize index = 0; index < entities.size(); ++index) {
            CY_REQUIRE(stores.push_back(&owned[index]).has_value());
        }
    }
};

[[nodiscard]] PerceptionTarget enemy(u32 index, Vec3 position, f32 loudness = 0.0F) noexcept {
    PerceptionTarget target;
    target.entity = Entity::make(1000u + index, 1);
    target.position = position;
    target.faction = 2;
    target.loudness = loudness;
    target.relevance = 1.0F;
    return target;
}

}  // namespace

CY_TEST_CASE("one observer sees a target in front of it and not one behind it") {
    Observers observers(allocator());
    observers.add(Vec3{}, AiTier::Full, 1.0F);
    observers.bind();

    Array<PerceptionTarget> targets(allocator());
    CY_REQUIRE(targets.push_back(enemy(0, Vec3{5.0F, 0.0F, 0.0F})).has_value());
    CY_REQUIRE(targets.push_back(enemy(1, Vec3{-5.0F, 0.0F, 0.0F})).has_value());

    PerceptionParams params;
    PerceptionScheduler scheduler(allocator(), params);
    CountingHost host;
    PerceptionReport report;
    CY_REQUIRE(scheduler.update(1, observers.columns(), targets.span(), host, report).has_value());

    CY_CHECK_EQ(report.sensors_due, 1U);
    CY_CHECK_EQ(report.rejected_angle, 1U);
    CY_CHECK_EQ(report.queries_issued, 1U);
    CY_CHECK_EQ(host.calls, 1U);
    CY_CHECK_EQ(report.sightings, 1U);
    CY_CHECK_EQ(observers.owned[0].size(), 1U);
    CY_CHECK(observers.owned[0].find(targets[0].entity) != nullptr);
}

CY_TEST_CASE("a target beyond the sight range is rejected before any query is issued") {
    Observers observers(allocator());
    observers.add(Vec3{}, AiTier::Full, 1.0F);
    observers.bind();
    Array<PerceptionTarget> targets(allocator());
    CY_REQUIRE(targets.push_back(enemy(0, Vec3{400.0F, 0.0F, 0.0F})).has_value());

    PerceptionScheduler scheduler(allocator(), PerceptionParams{});
    CountingHost host;
    PerceptionReport report;
    CY_REQUIRE(scheduler.update(1, observers.columns(), targets.span(), host, report).has_value());
    CY_CHECK_EQ(report.rejected_range, 1U);
    CY_CHECK_EQ(report.queries_issued, 0U);
    CY_CHECK_EQ(host.calls, 0U);
}

CY_TEST_CASE("hearing needs no query, and proximity needs no line of sight") {
    Observers observers(allocator());
    observers.add(Vec3{}, AiTier::Full, 1.0F);
    observers.forward[0] = Vec3{-1.0F, 0.0F, 0.0F};  // facing away from everything
    observers.bind();

    Array<PerceptionTarget> targets(allocator());
    CY_REQUIRE(targets.push_back(enemy(0, Vec3{8.0F, 0.0F, 0.0F}, 1.0F)).has_value());
    CY_REQUIRE(targets.push_back(enemy(1, Vec3{1.0F, 0.0F, 0.0F})).has_value());

    PerceptionScheduler scheduler(allocator(), PerceptionParams{});
    CountingHost host;
    PerceptionReport report;
    CY_REQUIRE(scheduler.update(1, observers.columns(), targets.span(), host, report).has_value());
    // Both were perceived; neither was traced.
    CY_CHECK_EQ(report.queries_issued, 0U);
    CY_CHECK_EQ(report.sounds, 1U);
    CY_CHECK_EQ(observers.owned[0].size(), 2U);
    CY_CHECK_EQ(observers.owned[0].find(targets[0].entity)->sense, SenseKind::Hearing);
    CY_CHECK_EQ(observers.owned[0].find(targets[1].entity)->sense, SenseKind::Proximity);
}

CY_TEST_CASE("a thousand observers issue a bounded, batched set of queries rather than one each") {
    // `ai-system`'s own scenario, scaled down by ten: "WHEN 10,000 agents have vision sensors THEN
    // the scheduler SHALL issue a BOUNDED, BATCHED set of queries rather than one or more raycasts
    // per agent per tick."
    Observers observers(allocator());
    for (u32 index = 0; index < 1000; ++index) {
        // Twenty-five to a cell, so sharing has something to share.
        const u32 row = index / 40u;
        observers.add(
            Vec3{static_cast<f32>(index % 40u) * 0.5F, 0.0F, static_cast<f32>(row) * 0.5F},
            AiTier::Full, 1.0F);
    }
    observers.bind();
    Array<PerceptionTarget> targets(allocator());
    for (u32 index = 0; index < 8; ++index) {
        CY_REQUIRE(targets
                       .push_back(enemy(index, Vec3{12.0F + static_cast<f32>(index), 0.0F,
                                                    static_cast<f32>(index)}))
                       .has_value());
    }

    PerceptionParams params;
    params.query_budget = 256;
    params.sharing_cell = 4.0F;
    PerceptionScheduler scheduler(allocator(), params);
    CountingHost host;
    PerceptionReport report;
    CY_REQUIRE(scheduler.update(1, observers.columns(), targets.span(), host, report).has_value());

    // ONE call to the host for the whole tick, and far fewer queries than candidates.
    CY_CHECK_EQ(host.calls, 1U);
    CY_CHECK_GT(report.candidates, 1000U);
    CY_CHECK_LE(report.queries_issued, params.query_budget);
    CY_CHECK_GT(report.queries_shared, 0U);
    // Sharing is what carried the rest: every candidate was either its own query or another's.
    CY_CHECK_EQ(report.candidates, report.queries_issued + report.queries_shared + report.deferred);
}

CY_TEST_CASE("the budget defers by importance and reports it, without dropping anybody") {
    Observers observers(allocator());
    // Two important observers and thirty ordinary ones, each looking at its own target so nothing
    // is shared and the budget is the only thing that can bind.
    for (u32 index = 0; index < 32; ++index) {
        observers.add(Vec3{0.0F, 0.0F, static_cast<f32>(index) * 20.0F}, AiTier::Full,
                      (index < 2) ? 10.0F : 1.0F);
    }
    observers.bind();
    Array<PerceptionTarget> targets(allocator());
    for (u32 index = 0; index < 32; ++index) {
        CY_REQUIRE(
            targets.push_back(enemy(index, Vec3{5.0F, 0.0F, static_cast<f32>(index) * 20.0F}))
                .has_value());
    }

    PerceptionParams params;
    params.query_budget = 4;
    params.share_queries = false;
    PerceptionScheduler scheduler(allocator(), params);
    CountingHost host;
    PerceptionReport report;
    CY_REQUIRE(scheduler.update(1, observers.columns(), targets.span(), host, report).has_value());

    CY_CHECK(report.budget_exceeded);
    CY_CHECK_EQ(report.queries_issued, 4U);
    CY_CHECK_GT(report.deferred, 0U);
    // The two important observers are among those that were served: prioritisation, not truncation.
    CY_CHECK_EQ(observers.owned[0].size(), 1U);
    CY_CHECK_EQ(observers.owned[1].size(), 1U);
}

CY_TEST_CASE("a reduced tier senses on a rotation and a minimal one senses not at all") {
    // `ai-system`'s tier table: `Reduced` gets cheap sensors on a slower rate, `Minimal` gets none
    // and takes its knowledge from shared channels.
    Observers observers(allocator());
    for (u32 index = 0; index < 12; ++index) {
        observers.add(Vec3{0.0F, 0.0F, static_cast<f32>(index) * 30.0F}, AiTier::Reduced, 1.0F);
    }
    observers.add(Vec3{0.0F, 0.0F, 1000.0F}, AiTier::Minimal, 1.0F);
    observers.bind();
    Array<PerceptionTarget> targets(allocator());
    for (u32 index = 0; index < 13; ++index) {
        CY_REQUIRE(
            targets.push_back(enemy(index, Vec3{5.0F, 0.0F, static_cast<f32>(index) * 30.0F}))
                .has_value());
    }

    PerceptionScheduler scheduler(allocator(), PerceptionParams{});
    CountingHost host;
    u32 total_due = 0;
    for (u32 tick = 0; tick < 6; ++tick) {
        PerceptionReport report;
        CY_REQUIRE(
            scheduler.update(tick, observers.columns(), targets.span(), host, report).has_value());
        // Twelve reduced agents over an interval of six: two per tick, never twelve.
        CY_CHECK_EQ(report.sensors_due, 2U);
        total_due += report.sensors_due;
    }
    // Every reduced agent had exactly one turn in the interval, and the minimal one had none.
    CY_CHECK_EQ(total_due, 12U);
    CY_CHECK_EQ(observers.owned[12].size(), 0U);
}

CY_TEST_CASE("the direct path answers now, outside the rotation and outside the budget") {
    // `ai-system`: "A direct, unbatched query path SHALL remain available for cases needing exact
    // instantaneous results, documented as expensive."
    Observers observers(allocator());
    observers.add(Vec3{}, AiTier::Minimal, 1.0F);  // a tier the rotation would never serve
    observers.bind();
    Array<PerceptionTarget> targets(allocator());
    CY_REQUIRE(targets.push_back(enemy(0, Vec3{5.0F, 0.0F, 0.0F})).has_value());

    PerceptionParams params;
    params.query_budget = 0;  // the batched path could issue nothing at all
    PerceptionScheduler scheduler(allocator(), params);
    CountingHost host;
    PerceptionReport report;
    CY_REQUIRE(scheduler
                   .sense_one(1, observers.entities[0], observers.positions[0],
                              observers.forward[0], observers.sensors[0], targets.span(),
                              observers.owned[0], host, report)
                   .has_value());
    CY_CHECK_EQ(report.queries_issued, 1U);
    CY_CHECK_EQ(report.sightings, 1U);
    CY_CHECK_EQ(observers.owned[0].size(), 1U);
}

CY_TEST_CASE("columns of different lengths are refused rather than read past their end") {
    Observers observers(allocator());
    observers.add(Vec3{}, AiTier::Full, 1.0F);
    observers.bind();
    Array<PerceptionTarget> targets(allocator());
    PerceptionScheduler scheduler(allocator(), PerceptionParams{});
    CountingHost host;
    PerceptionReport report;
    ObserverColumns broken = observers.columns();
    broken.positions = Span<const Vec3>{};
    const Status refused = scheduler.update(1, broken, targets.span(), host, report);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::InvalidArgument);
}
