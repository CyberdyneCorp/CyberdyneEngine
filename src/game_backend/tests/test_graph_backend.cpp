// SPDX-License-Identifier: MIT
// `integration.game_backend_graph`: gameplay graphs run on scene entities. Issue #29, visual
// scripting.
//
// The graph under test is the one the editor writes for the acceptance scenario —
// `src/editor_backend/tests/data/script_unit_command_v1.cyscript`, "on command, move to the target,
// then play the arrival cue" — read from the same file the editor's suites compare their output
// with, so a graph the editor authors and the graph these cases run cannot drift apart.

#include <cy/abi/cy_abi.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/game_backend/graph_behaviours.h>
#include <cy/scene/node.h>
#include <cy/scene/tree.h>
#include <cy/test/test.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "graph_scene.h"

namespace {

using namespace cy;
using game_backend::GraphBackend;
using game_backend::GraphInstanceStatus;
using test_graph::allocator;
using test_graph::fixture;
using test_graph::kDt;
using test_graph::RecordingAudio;
using test_graph::replaced;
using test_graph::Scene;

}  // namespace

CY_TEST_CASE("graph behaviours: an ordered unit moves to the target and plays its cue on arrival") {
    Scene scene;
    const ecs::Entity unit = scene.unit("Tank");
    const u32 graph =
        scene.load(GraphBackend::Bytecode, fixture("script_unit_command_v1.cyscript"));
    CY_REQUIRE(scene.graphs.attach(graph, unit).has_value());

    CY_CHECK_EQ(scene.order(unit, 6.0F, 8.0F), 1U);
    CY_CHECK(scene.graphs.instance(0).status == GraphInstanceStatus::Waiting);
    CY_CHECK_EQ(scene.graphs.instance(0).waiting, Name::intern("unit.arrived"));
    CY_CHECK(scene.audio.plays.empty());

    // Ten metres at the default three metres a second is two hundred ticks of a sixtieth.
    u64 arrived = 0;
    for (u32 step = 0; step < 400 && arrived == 0; ++step) {
        CY_REQUIRE(scene.graphs.update(kDt).has_value());
        const Vec3 here = scene.at(unit);
        CY_CHECK_LE(std::sqrt((here.x * here.x) + (here.z * here.z)), 10.0F + 1e-4F);
        if (!scene.graphs.instance(0).moving) {
            arrived = scene.graphs.tick();
        } else {
            CY_CHECK(scene.audio.plays.empty());
        }
    }
    CY_CHECK_GE(arrived, 199U);
    CY_CHECK_LE(arrived, 201U);
    CY_CHECK_EQ(scene.at(unit).x, 6.0F);
    CY_CHECK_EQ(scene.at(unit).z, 8.0F);

    // The cue played once, on the arrival tick, where the unit stopped, through the audio adapter.
    CY_REQUIRE_EQ(scene.audio.plays.size(), 1U);
    CY_CHECK_EQ(scene.audio.plays[0].cue, RecordingAudio::kArrived);
    CY_CHECK(scene.audio.plays[0].spatial);
    CY_CHECK_EQ(scene.audio.plays[0].x, 6.0F);
    CY_CHECK_EQ(scene.audio.plays[0].z, 8.0F);
    CY_REQUIRE_EQ(scene.graphs.cues().size(), 1U);
    CY_CHECK_EQ(scene.graphs.cues()[0].tick, arrived);
    CY_CHECK_EQ(scene.graphs.cues()[0].cue, Name::intern("unit.arrived"));
    CY_CHECK(scene.graphs.instance(0).status == GraphInstanceStatus::Idle);

    // Nothing more happens without another event: events, not a tick.
    for (u32 step = 0; step < 30; ++step) {
        CY_REQUIRE(scene.graphs.update(kDt).has_value());
    }
    CY_CHECK_EQ(scene.audio.plays.size(), 1U);
}

CY_TEST_CASE("graph behaviours: an event the graph does not answer starts nothing") {
    Scene scene;
    const ecs::Entity unit = scene.unit("Tank");
    const u32 graph =
        scene.load(GraphBackend::Bytecode, fixture("script_unit_command_v1.cyscript"));
    CY_REQUIRE(scene.graphs.attach(graph, unit).has_value());
    const auto started = scene.graphs.raise(unit, Name::intern("unit.selected"), {});
    CY_REQUIRE(started.has_value());
    CY_CHECK_EQ(*started, 0U);
    CY_CHECK_EQ(scene.graphs.instance(0).runs, 0U);
    CY_CHECK(!scene.graphs.instance(0).moving);
}

CY_TEST_CASE("graph behaviours: a newer order replaces the one the unit is still carrying out") {
    Scene scene;
    const ecs::Entity unit = scene.unit("Tank");
    const u32 graph =
        scene.load(GraphBackend::Bytecode, fixture("script_unit_command_v1.cyscript"));
    CY_REQUIRE(scene.graphs.attach(graph, unit).has_value());
    CY_CHECK_EQ(scene.order(unit, 30.0F, 0.0F), 1U);
    for (u32 step = 0; step < 20; ++step) {
        CY_REQUIRE(scene.graphs.update(kDt).has_value());
    }
    CY_CHECK_EQ(scene.order(unit, -2.0F, 0.0F), 1U);
    for (u32 step = 0; step < 200 && scene.graphs.instance(0).moving; ++step) {
        CY_REQUIRE(scene.graphs.update(kDt).has_value());
    }
    CY_CHECK_EQ(scene.at(unit).x, -2.0F);
    CY_REQUIRE_EQ(scene.audio.plays.size(), 1U);
    CY_CHECK_EQ(scene.audio.plays[0].x, -2.0F);
    CY_CHECK_EQ(scene.graphs.instance(0).runs, 2U);
}

CY_TEST_CASE("graph behaviours: one compiled program serves a hundred units, each with its state") {
    Scene scene;
    const u32 graph =
        scene.load(GraphBackend::Bytecode, fixture("script_unit_command_v1.cyscript"));
    std::vector<ecs::Entity> units;
    for (u32 index = 0; index < 100; ++index) {
        units.push_back(scene.unit("Unit"));
        CY_REQUIRE(scene.graphs.attach(graph, units.back()).has_value());
    }
    CY_CHECK_EQ(scene.graphs.graph_count(), 1U);
    CY_CHECK_EQ(scene.graphs.instance_count(), 100U);
    for (u32 index = 0; index < 100; ++index) {
        CY_CHECK_EQ(scene.order(units[index], static_cast<f32>(index % 10), 1.0F), 1U);
    }
    for (u32 step = 0; step < 400; ++step) {
        CY_REQUIRE(scene.graphs.update(kDt).has_value());
    }
    for (u32 index = 0; index < 100; ++index) {
        CY_CHECK_EQ(scene.at(units[index]).x, static_cast<f32>(index % 10));
        CY_CHECK_EQ(scene.at(units[index]).z, 1.0F);
    }
    CY_CHECK_EQ(scene.audio.plays.size(), 100U);
}

CY_TEST_CASE("graph behaviours: the bytecode and native back ends move and sound tick for tick") {
    Scene scene;
    const std::string source = fixture("script_unit_command_v1.cyscript");
    const u32 bytecode = scene.load(GraphBackend::Bytecode, source);
    const u32 native = scene.load(GraphBackend::Native, source);
    CY_CHECK_EQ(scene.graphs.program(bytecode)->program().digest(),
                scene.graphs.program(native)->program().digest());
    const ecs::Entity first = scene.unit("Bytecode");
    const ecs::Entity second = scene.unit("Native");
    CY_REQUIRE(scene.graphs.attach(bytecode, first).has_value());
    CY_REQUIRE(scene.graphs.attach(native, second).has_value());
    CY_CHECK_EQ(scene.order(first, 7.5F, -3.25F), 1U);
    CY_CHECK_EQ(scene.order(second, 7.5F, -3.25F), 1U);
    for (u32 step = 0; step < 300; ++step) {
        CY_REQUIRE(scene.graphs.update(kDt).has_value());
        // EXACTLY, not within a tolerance: one IR, one host, and so one sequence of floats.
        CY_CHECK_EQ(scene.at(first).x, scene.at(second).x);
        CY_CHECK_EQ(scene.at(first).z, scene.at(second).z);
        CY_CHECK_EQ(scene.graphs.instance(0).moving, scene.graphs.instance(1).moving);
    }
    CY_REQUIRE_EQ(scene.graphs.cues().size(), 2U);
    CY_CHECK_EQ(scene.graphs.cues()[0].tick, scene.graphs.cues()[1].tick);
    CY_CHECK_EQ(scene.graphs.cues()[0].position.x, scene.graphs.cues()[1].position.x);
    CY_CHECK_EQ(scene.graphs.cues()[0].position.z, scene.graphs.cues()[1].position.z);
}

CY_TEST_CASE("graph behaviours: a cue the project lacks is refused on the node that plays it") {
    Scene scene;
    const std::string source = replaced(fixture("script_unit_command_v1.cyscript"),
                                        "cue.unit.arrived", "cue.unit.vanished");
    graph::DiagnosticSink sink(allocator());
    const auto loaded =
        scene.graphs.load(Name::intern("unit_command"), source, GraphBackend::Bytecode, sink);
    CY_CHECK(!loaded.has_value());
    CY_REQUIRE_EQ(sink.entries().size(), 1U);
    CY_CHECK_EQ(std::string(sink.entries()[0].code), "script.cue.unknown");
    CY_CHECK_EQ(sink.entries()[0].node, 6U);
    CY_CHECK_EQ(sink.entries()[0].detail, Name::intern("unit.vanished"));
}

CY_TEST_CASE("graph behaviours: a function the engine does not declare is an error on its node") {
    Scene scene;
    const std::string source =
        replaced(fixture("script_unit_command_v1.cyscript"), "unit.move_to", "unit.mvoe_to");
    graph::DiagnosticSink sink(allocator());
    const auto loaded =
        scene.graphs.load(Name::intern("unit_command"), source, GraphBackend::Bytecode, sink);
    CY_CHECK(!loaded.has_value());
    CY_REQUIRE_EQ(sink.errors(), 1U);
    CY_CHECK_EQ(std::string(sink.entries()[0].code), "script.external.unknown");
    CY_CHECK_EQ(sink.entries()[0].node, 4U);
    CY_CHECK_EQ(sink.entries()[0].detail, Name::intern("unit.mvoe_to"));
    CY_CHECK_EQ(scene.graphs.graph_count(), 0U);
}

CY_TEST_CASE("graph behaviours: the step is the one statement of a unit's arithmetic") {
    Vec3 position{1.0F, 2.0F, 1.0F};
    CY_CHECK(!game_backend::step_towards(position, 4.0F, 5.0F, 3.0F, 1.0F));
    CY_CHECK_EQ(position.x, 1.0F + (3.0F / 5.0F * 3.0F));
    CY_CHECK_EQ(position.z, 1.0F + (4.0F / 5.0F * 3.0F));
    CY_CHECK_EQ(position.y, 2.0F);
    CY_CHECK(game_backend::step_towards(position, 4.0F, 5.0F, 3.0F, 1.0F));
    CY_CHECK_EQ(position.x, 4.0F);
    CY_CHECK_EQ(position.z, 5.0F);
}

namespace {

/// `step_towards` with every product and quotient rounded to a float before it is added, as Swift
/// (which never contracts) evaluates the twin's expression. The volatile stores keep this file's
/// own compiler from fusing them, whatever its contraction default.
bool step_rounded(Vec3& position, f32 target_x, f32 target_z, f32 speed, f32 dt) noexcept {
    const f32 dx = target_x - position.x;
    const f32 dz = target_z - position.z;
    const volatile f32 dx2 = dx * dx;
    const volatile f32 dz2 = dz * dz;
    const f32 distance = std::sqrt(dx2 + dz2);
    const f32 step = speed * dt;
    if (distance <= step) {
        position.x = target_x;
        position.z = target_z;
        return true;
    }
    const volatile f32 along_x = dx / distance * step;
    const volatile f32 along_z = dz / distance * step;
    position.x = position.x + along_x;
    position.z = position.z + along_z;
    return false;
}

}  // namespace

CY_TEST_CASE("graph behaviours: the step never fuses a multiply into the add that follows it") {
    // The acceptance unit's walk, ten metres at three a second in sixtieths: on a host with a fused
    // multiply-add (aarch64, Apple silicon) a contracted `x += dx / distance * step` leaves the
    // rounded walk by an ULP from the twentieth tick, and the graph unit then disagrees with its
    // Swift twin.
    const f32 dt = static_cast<f32>(1.0 / 60.0);
    Vec3 engine{0.0F, 0.0F, 0.0F};
    Vec3 rounded{0.0F, 0.0F, 0.0F};
    for (u32 tick = 1; tick <= 260; ++tick) {
        CY_TEST_INFO("tick " << tick);
        const bool engine_arrived = game_backend::step_towards(engine, 6.0F, 8.0F, 3.0F, dt);
        const bool rounded_arrived = step_rounded(rounded, 6.0F, 8.0F, 3.0F, dt);
        CY_REQUIRE_EQ(engine.x, rounded.x);
        CY_REQUIRE_EQ(engine.z, rounded.z);
        CY_REQUIRE_EQ(engine_arrived, rounded_arrived);
    }
    CY_CHECK_EQ(engine.x, 6.0F);
    CY_CHECK_EQ(engine.z, 8.0F);
}
