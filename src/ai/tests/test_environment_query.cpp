// Environment queries: generators, weighted tests, deterministic budget reduction and ranking.
// M8.b task 6.4.

#include <cy/ai/environment_query.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cmath>

using namespace cy;
using namespace cy::ai;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// A host with a wall: anything on the far side of x = 0 from the threat is out of sight, and
/// everything is reachable at a cost equal to the straight-line distance.
class WalledHost final : public QueryHost {
public:
    void trace_batch(Span<const Vec3> from, Span<const Vec3> to, Span<bool> results) override {
        ++trace_calls;
        for (usize index = 0; index < results.size(); ++index) {
            const bool crosses_wall = (from[index].x < 0.0F) != (to[index].x < 0.0F);
            results[index] = !crosses_wall;
        }
    }

    void path_cost_batch(Vec3 from, Span<const Vec3> to, Span<f32> costs) override {
        ++path_calls;
        for (usize index = 0; index < costs.size(); ++index) {
            const Vec3 offset = to[index] - from;
            const f32 distance = std::sqrt((offset.x * offset.x) + (offset.z * offset.z));
            costs[index] = (distance > unreachable_beyond) ? -1.0F : distance;
        }
    }

    u32 trace_calls = 0;
    u32 path_calls = 0;
    f32 unreachable_beyond = 1000.0F;
};

[[nodiscard]] QueryTest distance_from(Vec3 reference, f32 span, bool invert) noexcept {
    QueryTest test;
    test.kind = TestKind::Distance;
    test.reference = reference;
    test.span = span;
    test.invert = invert;
    return test;
}

}  // namespace

CY_TEST_CASE("every generator produces the candidates its shape implies") {
    EnvironmentQuery query(allocator(), 4096);
    CY_REQUIRE(query.generate_grid(Vec3{}, 2.0F, 1.0F).has_value());
    CY_CHECK_EQ(query.items().size(), usize{25});  // 5 x 5

    query.clear();
    CY_REQUIRE(query.generate_ring(Vec3{}, 2.0F, 6.0F, 3, 8).has_value());
    CY_CHECK_EQ(query.items().size(), usize{24});

    query.clear();
    CY_REQUIRE(query.generate_around(Vec3{1.0F, 0.0F, 1.0F}, 3.0F, 6).has_value());
    CY_CHECK_EQ(query.items().size(), usize{6});

    query.clear();
    const Entity actors[3] = {Entity::make(1, 1), Entity::make(2, 1), Entity::make(3, 1)};
    const Vec3 positions[3] = {Vec3{1.0F, 0.0F, 0.0F}, Vec3{4.0F, 0.0F, 0.0F},
                               Vec3{40.0F, 0.0F, 0.0F}};
    CY_REQUIRE(query.generate_actors({actors, 3}, {positions, 3}, Vec3{}, 10.0F).has_value());
    CY_CHECK_EQ(query.items().size(), usize{2});
    CY_CHECK_EQ(query.items()[0].actor, actors[0]);

    query.clear();
    const Vec3 custom[2] = {Vec3{7.0F, 0.0F, 7.0F}, Vec3{8.0F, 0.0F, 8.0F}};
    CY_REQUIRE(query.generate_custom({custom, 2}).has_value());
    CY_CHECK_EQ(query.items().size(), usize{2});

    // A degenerate generator is refused rather than producing nothing quietly.
    query.clear();
    CY_CHECK_FALSE(query.generate_grid(Vec3{}, 2.0F, 0.0F).has_value());
    CY_CHECK_FALSE(query.generate_ring(Vec3{}, 1.0F, 2.0F, 0, 4).has_value());
}

CY_TEST_CASE("finding cover scores by distance, line of sight and reachability") {
    // `ai-system`'s worked example: "generate candidate points, score them by distance, line of
    // sight to the threat, and reachability, and return the best".
    EnvironmentQuery query(allocator(), 256);
    const Vec3 threat{6.0F, 0.0F, 0.0F};
    CY_REQUIRE(query.generate_grid(Vec3{}, 4.0F, 2.0F).has_value());

    // Close to me.
    CY_REQUIRE(query.add_test(distance_from(Vec3{}, 12.0F, false)).has_value());
    // Out of the threat's sight, and that one is a FILTER: cover an enemy can see is not cover.
    QueryTest hidden;
    hidden.kind = TestKind::LineOfSight;
    hidden.reference = threat;
    hidden.invert = true;  // score high when the trace is BLOCKED
    hidden.filter_below = 0.5F;
    hidden.weight = 1.0F;
    CY_REQUIRE(query.add_test(hidden).has_value());
    // And I have to be able to get there.
    QueryTest reachable;
    reachable.kind = TestKind::Reachable;
    reachable.filter_below = 0.5F;
    CY_REQUIRE(query.add_test(reachable).has_value());

    WalledHost host;
    EnvironmentQueryReport report;
    CY_REQUIRE(query.run(Vec3{-3.0F, 0.0F, 0.0F}, host, report).has_value());
    CY_CHECK_EQ(report.candidates_generated, 25U);
    CY_CHECK_EQ(host.trace_calls, 1U);  // ONE batch, not one per candidate
    CY_CHECK_EQ(host.path_calls, 1U);
    CY_CHECK_GT(report.candidates_rejected, 0U);

    CY_REQUIRE(query.best() != nullptr);
    // Cover is on the near side of the wall: the threat is at x > 0, so every surviving candidate
    // is at x < 0.
    CY_CHECK_LT(query.best()->position.x, 0.0F);
    for (const u32 index : query.ranked()) {
        CY_REQUIRE(query.items()[index].position.x < 0.0F);
    }
    // And the best is the nearest of them.
    CY_CHECK_GE(query.best()->score,
                query.items()[query.ranked()[query.ranked().size() - 1]].score);
}

CY_TEST_CASE("a budget reduces candidates by a stride and reports it") {
    // `ai-system`: "candidates SHALL be REDUCED DETERMINISTICALLY before scoring, and the reduction
    // REPORTED". A stride rather than a truncation, so the reduced set still covers the shape.
    EnvironmentQuery query(allocator(), 20);
    CY_REQUIRE(query.generate_grid(Vec3{}, 10.0F, 1.0F).has_value());  // 21 x 21 = 441
    CY_REQUIRE(query.add_test(distance_from(Vec3{}, 20.0F, false)).has_value());

    WalledHost host;
    EnvironmentQueryReport report;
    CY_REQUIRE(query.run(Vec3{}, host, report).has_value());
    CY_CHECK(report.budget_exceeded);
    CY_CHECK_EQ(report.candidates_generated, 441U);
    CY_CHECK_GT(report.stride, 1U);
    CY_CHECK_LE(query.items().size(), usize{20});
    CY_CHECK_EQ(report.candidates_dropped, 441U - static_cast<u32>(query.items().size()));

    // The kept set still spans the grid rather than one corner of it, which is the whole reason a
    // stride was chosen over a truncation.
    f32 lowest = 1000.0F;
    f32 highest = -1000.0F;
    for (const QueryItem& item : query.items()) {
        lowest = (item.position.z < lowest) ? item.position.z : lowest;
        highest = (item.position.z > highest) ? item.position.z : highest;
    }
    CY_CHECK_LT(lowest, -5.0F);
    CY_CHECK_GT(highest, 5.0F);
}

CY_TEST_CASE("a query is deterministic and its ranking has a stable tie-break") {
    const auto run = [](Array<Vec3>& out) noexcept {
        EnvironmentQuery query(allocator(), 64);
        CY_REQUIRE(query.generate_ring(Vec3{}, 3.0F, 3.0F, 1, 8).has_value());
        CY_REQUIRE(query.add_test(distance_from(Vec3{}, 10.0F, false)).has_value());
        WalledHost host;
        EnvironmentQueryReport report;
        CY_REQUIRE(query.run(Vec3{}, host, report).has_value());
        for (const u32 index : query.ranked()) {
            CY_REQUIRE(out.push_back(query.items()[index].position).has_value());
        }
    };
    Array<Vec3> first(allocator());
    Array<Vec3> second(allocator());
    run(first);
    run(second);
    CY_REQUIRE_EQ(first.size(), second.size());
    // Every candidate on a ring is the same distance from the centre, so every score ties — and the
    // ranking still has to be the same in both runs.
    for (usize index = 0; index < first.size(); ++index) {
        CY_REQUIRE_EQ(first[index].x, second[index].x);
        CY_REQUIRE_EQ(first[index].z, second[index].z);
    }
}

CY_TEST_CASE("a custom test is invoked for every surviving candidate") {
    // `ai-system`'s "registration point for custom tests".
    struct Counter {
        u32 calls = 0;
    };
    Counter counter;
    const CustomTestFn prefer_east = [](const QueryItem& item, void* user) noexcept {
        ++static_cast<Counter*>(user)->calls;
        return (item.position.x > 0.0F) ? 1.0F : 0.0F;
    };

    EnvironmentQuery query(allocator(), 64);
    CY_REQUIRE(query.generate_grid(Vec3{}, 2.0F, 1.0F).has_value());
    graph::behaviour::ResponseCurve curve;
    CY_REQUIRE(query.add_custom_test(prefer_east, &counter, 1.0F, curve).has_value());

    WalledHost host;
    EnvironmentQueryReport report;
    CY_REQUIRE(query.run(Vec3{}, host, report).has_value());
    CY_CHECK_EQ(counter.calls, 25U);
    CY_REQUIRE(query.best() != nullptr);
    CY_CHECK_GT(query.best()->position.x, 0.0F);
    CY_CHECK_FALSE(query.add_custom_test(nullptr, nullptr, 1.0F, curve).has_value());
}

CY_TEST_CASE("a path-cost test ranks the cheapest route and rejects what is unreachable") {
    EnvironmentQuery query(allocator(), 64);
    CY_REQUIRE(query.generate_ring(Vec3{}, 2.0F, 12.0F, 4, 4).has_value());
    QueryTest cheap;
    cheap.kind = TestKind::PathCost;
    cheap.span = 20.0F;
    CY_REQUIRE(query.add_test(cheap).has_value());
    QueryTest reachable;
    reachable.kind = TestKind::Reachable;
    reachable.filter_below = 0.5F;
    CY_REQUIRE(query.add_test(reachable).has_value());

    WalledHost host;
    host.unreachable_beyond = 8.0F;
    EnvironmentQueryReport report;
    CY_REQUIRE(query.run(Vec3{}, host, report).has_value());
    CY_CHECK_GT(report.candidates_rejected, 0U);
    CY_REQUIRE(query.best() != nullptr);
    const Vec3 best = query.best()->position;
    CY_CHECK_LE(std::sqrt((best.x * best.x) + (best.z * best.z)), 8.0F);
}
