// Behaviours bridging nodes and systems: the batching decision, the report, and the dispatch.
// Task 3.1.7.

#include <cy/test/test.h>

#include <cy/ecs/system.h>
#include <cy/scene/behaviour.h>
#include <cy/scene/tree.h>

#include "fixtures.h"

#include <cstring>

using cy::scene::test::Fixture;
using cy::scene::test::Health;
using cy::scene::test::make_child;

namespace {

/// The component id the batched body reads out of the chunk. A file-scope value because a behaviour
/// body is a function pointer: this is what a generated system would have baked in.
cy::ecs::ComponentTypeId g_health = cy::ecs::kInvalidComponent;
cy::u32 g_batch_calls = 0;
cy::u32 g_instance_calls = 0;

/// The lowered form: one call per chunk, iterating a contiguous column.
void mover_batch(const cy::scene::BehaviourBatch& batch) noexcept {
    ++g_batch_calls;
    const cy::Span<Health> health = batch.chunk->write<Health>(g_health);
    for (Health& value : health) {
        ++value.value;
    }
}

/// The per-instance form: one call per entity.
void scripted_update(const cy::scene::BehaviourContext& context) noexcept {
    ++g_instance_calls;
    auto* health = static_cast<Health*>(context.node.get_mut(g_health));
    if (health != nullptr) {
        health->value += 10;
    }
}

[[nodiscard]] cy::scene::BehaviourDesc mover() noexcept {
    cy::scene::BehaviourDesc desc;
    desc.name = "Mover";
    desc.update_batch = &mover_batch;
    desc.writes = cy::Span<const cy::ecs::ComponentTypeId>(&g_health, 1);
    return desc;
}

[[nodiscard]] cy::scene::BehaviourDesc scripted() noexcept {
    cy::scene::BehaviourDesc desc;
    desc.name = "Scripted";
    desc.on_update = &scripted_update;
    desc.writes = cy::Span<const cy::ecs::ComponentTypeId>(&g_health, 1);
    // The first of the three disqualifiers the specification names.
    desc.invokes_script = true;
    return desc;
}

}  // namespace

CY_TEST_CASE("the batching decision is a function of the declaration alone") {
    cy::scene::BehaviourDesc desc;
    desc.name = "Probe";
    const cy::ecs::ComponentTypeId component = 0;
    desc.writes = cy::Span<const cy::ecs::ComponentTypeId>(&component, 1);

    // No per-tick callback: in no per-frame list at all, which is what makes an unimplemented
    // callback cost nothing.
    CY_CHECK(cy::scene::decide_dispatch(desc).dispatch == cy::scene::BehaviourDispatch::None);

    desc.update_batch = &mover_batch;
    CY_CHECK(cy::scene::decide_dispatch(desc).dispatch == cy::scene::BehaviourDispatch::Batched);
    CY_CHECK(std::strlen(cy::scene::decide_dispatch(desc).reason) == 0);

    // The three disqualifiers, each with a reason a developer can act on.
    {
        cy::scene::BehaviourDesc probe = desc;
        probe.invokes_script = true;
        const cy::scene::DispatchDecision decision = cy::scene::decide_dispatch(probe);
        CY_CHECK(decision.dispatch == cy::scene::BehaviourDispatch::PerInstance);
        CY_CHECK(std::strlen(decision.reason) > 0);
    }
    {
        cy::scene::BehaviourDesc probe = desc;
        probe.state_size = 16;
        CY_CHECK(cy::scene::decide_dispatch(probe).dispatch ==
                 cy::scene::BehaviourDispatch::PerInstance);
    }
    {
        cy::scene::BehaviourDesc probe = desc;
        probe.accesses_undeclared_data = true;
        CY_CHECK(cy::scene::decide_dispatch(probe).dispatch ==
                 cy::scene::BehaviourDispatch::PerInstance);
    }
    // And two that are about the declaration being unbatchable in principle.
    {
        cy::scene::BehaviourDesc probe = desc;
        probe.writes = cy::Span<const cy::ecs::ComponentTypeId>();
        CY_CHECK(cy::scene::decide_dispatch(probe).dispatch ==
                 cy::scene::BehaviourDispatch::PerInstance);
    }
    {
        cy::scene::BehaviourDesc probe = desc;
        probe.on_fixed_update = &scripted_update;  // declared, with no lowered form
        CY_CHECK(cy::scene::decide_dispatch(probe).dispatch ==
                 cy::scene::BehaviourDispatch::PerInstance);
    }
}

CY_TEST_CASE("many instances of a batched behaviour cost one system, not one call each") {
    g_batch_calls = 0;
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const auto health =
        fixture.world.components().register_reflected(cy::scene::test::health_type());
    CY_REQUIRE(health.has_value());
    g_health = *health;

    const auto type = fixture.tree.behaviours().add(fixture.world, mover());
    CY_REQUIRE(type.has_value());
    CY_CHECK(fixture.tree.behaviours().dispatch_of(*type) == cy::scene::BehaviourDispatch::Batched);

    for (cy::u32 index = 0; index < 500; ++index) {
        cy::scene::Node node = make_child(fixture.tree, fixture.tree.root(), "Unit");
        CY_REQUIRE(node.valid());
        const Health initial{0};
        CY_REQUIRE(node.add(*health, &initial).has_value());
        CY_REQUIRE(fixture.tree.behaviours().attach(fixture.tree, node, *type).has_value());
    }
    CY_REQUIRE(fixture.tree.propagate().has_value());

    cy::ecs::Schedule schedule(fixture.world);
    CY_REQUIRE(fixture.tree.install_systems(schedule).has_value());
    CY_REQUIRE(schedule.build().has_value());
    // `scene-graph-and-nodes`: "behaviour execution SHALL occur through systems". The behaviour is
    // one system in `Frame`, beside the pre-render propagation.
    CY_CHECK_EQ(schedule.system_count(cy::ecs::Stage::Frame), 2U);

    CY_REQUIRE(schedule.run_serial(cy::ecs::Stage::Frame).has_value());
    // 500 instances, and the body ran once per chunk rather than 500 times.
    CY_CHECK_GT(g_batch_calls, 0U);
    CY_CHECK_LT(g_batch_calls, 500U);

    cy::u32 updated = 0;
    cy::Array<cy::scene::Node> children(cy::scene::test::allocator());
    CY_REQUIRE(fixture.tree.root().children(children).has_value());
    for (const cy::scene::Node node : children) {
        updated += (node.get_as<Health>(*health)->value == 1) ? 1U : 0U;
    }
    CY_CHECK_EQ(updated, 500U);
}

CY_TEST_CASE("a behaviour that cannot batch falls back to a system that dispatches per instance") {
    g_instance_calls = 0;
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const auto health =
        fixture.world.components().register_reflected(cy::scene::test::health_type());
    CY_REQUIRE(health.has_value());
    g_health = *health;

    const auto type = fixture.tree.behaviours().add(fixture.world, scripted());
    CY_REQUIRE(type.has_value());
    CY_CHECK(fixture.tree.behaviours().dispatch_of(*type) ==
             cy::scene::BehaviourDispatch::PerInstance);

    for (cy::u32 index = 0; index < 8; ++index) {
        cy::scene::Node node = make_child(fixture.tree, fixture.tree.root(), "Unit");
        const Health initial{0};
        CY_REQUIRE(node.add(*health, &initial).has_value());
        CY_REQUIRE(fixture.tree.behaviours().attach(fixture.tree, node, *type).has_value());
    }
    CY_REQUIRE(fixture.tree.propagate().has_value());

    cy::ecs::Schedule schedule(fixture.world);
    CY_REQUIRE(fixture.tree.install_systems(schedule).has_value());
    CY_REQUIRE(schedule.build().has_value());
    CY_REQUIRE(schedule.run_serial(cy::ecs::Stage::Frame).has_value());

    // "Per-instance dispatch SHALL remain a system iterating entities with a behaviour reference":
    // one system, eight calls.
    CY_CHECK_EQ(g_instance_calls, 8U);
    CY_CHECK_EQ(schedule.system_count(cy::ecs::Stage::Frame), 2U);
}

CY_TEST_CASE("a disabled node's behaviour is not dispatched") {
    g_instance_calls = 0;
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const auto health =
        fixture.world.components().register_reflected(cy::scene::test::health_type());
    g_health = *health;
    const auto type = fixture.tree.behaviours().add(fixture.world, scripted());
    CY_REQUIRE(type.has_value());

    cy::scene::Node keep = make_child(fixture.tree, fixture.tree.root(), "Keep");
    cy::scene::Node skip = make_child(fixture.tree, fixture.tree.root(), "Skip");
    for (cy::scene::Node node : {keep, skip}) {
        const Health initial{0};
        CY_REQUIRE(node.add(*health, &initial).has_value());
        CY_REQUIRE(fixture.tree.behaviours().attach(fixture.tree, node, *type).has_value());
    }
    CY_REQUIRE(skip.set_enabled(false).has_value());
    CY_REQUIRE(fixture.tree.propagate().has_value());

    cy::ecs::Schedule schedule(fixture.world);
    CY_REQUIRE(fixture.tree.install_systems(schedule).has_value());
    CY_REQUIRE(schedule.build().has_value());
    CY_REQUIRE(schedule.run_serial(cy::ecs::Stage::Frame).has_value());
    // The disabled node's chunk is excluded by the query, not tested per row.
    CY_CHECK_EQ(g_instance_calls, 1U);
}

CY_TEST_CASE("the report names every behaviour, its dispatch, and why") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const auto health =
        fixture.world.components().register_reflected(cy::scene::test::health_type());
    g_health = *health;
    CY_REQUIRE(fixture.tree.behaviours().add(fixture.world, mover()).has_value());
    CY_REQUIRE(fixture.tree.behaviours().add(fixture.world, scripted()).has_value());

    cy::Array<cy::scene::BehaviourReport> report(cy::scene::test::allocator());
    CY_REQUIRE(fixture.tree.behaviours().report(report).has_value());
    CY_REQUIRE_EQ(report.size(), 2U);

    CY_CHECK(report[0].dispatch == cy::scene::BehaviourDispatch::Batched);
    CY_CHECK_EQ(std::strlen(report[0].reason), 0U);
    // "The build report SHALL name it and the reason, rather than the cost appearing only at
    // scale."
    CY_CHECK(report[1].dispatch == cy::scene::BehaviourDispatch::PerInstance);
    CY_CHECK_GT(std::strlen(report[1].reason), 0U);

    cy::Array<char> text(cy::scene::test::allocator());
    CY_REQUIRE(fixture.tree.behaviours().write_report(text).has_value());
    const std::string_view rendered(text.data());
    CY_CHECK(rendered.find("Mover") != std::string_view::npos);
    CY_CHECK(rendered.find("Scripted") != std::string_view::npos);
    CY_CHECK(rendered.find("per-instance") != std::string_view::npos);
    CY_CHECK(rendered.find("reason:") != std::string_view::npos);
}
