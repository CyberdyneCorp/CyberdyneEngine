// The ECS's diagnostics on the M0 trace. Task 2.12.
//
// Integration rather than unit: it opens the one trace and writes a file.

#include <cy/test/test.h>

#include <cy/core/diagnostics/trace.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/diagnostics.h>
#include <cy/ecs/query.h>
#include <cy/ecs/system.h>
#include <cy/ecs/world.h>

#include <cstdio>

#include "fixtures.h"

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Ecs);
}

}  // namespace

CY_TEST_CASE("the ECS reports its shape, its systems and its queries onto the one trace") {
    cy::diag::TraceConfig config;
    config.path = "ecs_diagnostics.cytrace";
    config.consumer_thread = false;
    CY_REQUIRE(cy::diag::trace_open(config).has_value());

    {
        cy::ecs::World world(allocator());
        CY_REQUIRE(world.initialize().has_value());
        const auto ids = cy::ecs::test::register_all(world);
        CY_REQUIRE(ids.has_value());

        const cy::ecs::ComponentTypeId set[] = {ids->position, ids->velocity};
        cy::Array<cy::ecs::Entity> created(allocator());
        CY_REQUIRE(world.create_many(64, cy::Span<const cy::ecs::ComponentTypeId>(set, 2), created)
                       .has_value());

        cy::ecs::Schedule schedule(world);
        cy::ecs::SystemDesc desc;
        desc.name = "diagnostic-system";
        desc.body = [](const cy::ecs::SystemContext&) noexcept {};
        CY_REQUIRE(desc.access.write(ids->position).has_value());
        CY_REQUIRE(schedule.add(cy::ecs::Stage::Simulation, desc).has_value());
        CY_REQUIRE(schedule.build().has_value());
        CY_REQUIRE(schedule.run_serial(cy::ecs::Stage::Simulation).has_value());

        cy::ecs::QueryDesc query_desc(allocator());
        CY_REQUIRE(query_desc.read(ids->position).has_value());
        cy::ecs::Query query(world, std::move(query_desc));
        CY_REQUIRE(query.for_each_chunk([](cy::ecs::QueryChunk&) {}).has_value());

        cy::ecs::EcsDiagnostics diagnostics(world);
        diagnostics.report_world();
        diagnostics.report_systems(schedule);
        diagnostics.report_query("moving-entities", query.stats());
    }

    cy::diag::trace_flush();
    const auto stats = cy::diag::trace_close();
    CY_REQUIRE(stats.has_value());
    // Every field carries a classification; nothing here was refused as unclassified.
    CY_CHECK_EQ(stats->unclassified_fields, 0u);
    CY_CHECK_GT(stats->events_written, 0u);
    (void)std::remove("ecs_diagnostics.cytrace");
}

CY_TEST_CASE("archetype thrash is detected and names the component to declare sparse") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    auto entity = world.create();
    CY_REQUIRE(entity.has_value());
    CY_REQUIRE(world.add(*entity, ids->position).has_value());

    cy::ecs::ThrashPolicy policy;
    policy.transitions_per_window = 8;
    policy.window_ns = 1;  // one nanosecond, so the window closes on the next check
    cy::ecs::EcsDiagnostics diagnostics(world, policy);

    // Opens the window; nothing to report yet.
    CY_CHECK_FALSE(diagnostics.check_thrash(1000).detected());

    // A component gained and lost every frame is what the requirement's scenario describes.
    for (int round = 0; round < 6; ++round) {
        CY_REQUIRE(world.add(*entity, ids->frozen).has_value());
        CY_REQUIRE(world.remove(*entity, ids->frozen).has_value());
    }

    const cy::ecs::ThrashReport report = diagnostics.check_thrash(2000);
    CY_REQUIRE(report.detected());
    CY_CHECK_EQ(report.entity, *entity);
    CY_CHECK_EQ(report.component, ids->frozen);
    CY_CHECK(cy::ecs::test::same_text(report.component_name, "cy::ecs::test::Frozen"));
    CY_CHECK_EQ(report.transitions, 12u);

    // The counters are reset with the window, so a quiet second reports nothing.
    CY_CHECK_FALSE(diagnostics.check_thrash(3000).detected());
}

CY_TEST_CASE("a world under a sparse declaration does not thrash at all") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    auto entity = world.create();
    CY_REQUIRE(entity.has_value());
    CY_REQUIRE(world.add(*entity, ids->position).has_value());

    cy::ecs::ThrashPolicy policy;
    policy.transitions_per_window = 4;
    policy.window_ns = 1;
    cy::ecs::EcsDiagnostics diagnostics(world, policy);
    CY_CHECK_FALSE(diagnostics.check_thrash(1000).detected());

    // The advice the previous case emits, taken: the same toggle against a sparse component moves
    // no archetype and therefore produces no transitions to count.
    const cy::ecs::test::Selected selected{1};
    for (int round = 0; round < 20; ++round) {
        CY_REQUIRE(world.set_sparse(*entity, ids->selected, &selected).has_value());
        CY_REQUIRE(world.remove_sparse(*entity, ids->selected).has_value());
    }
    CY_CHECK_FALSE(diagnostics.check_thrash(2000).detected());
}
