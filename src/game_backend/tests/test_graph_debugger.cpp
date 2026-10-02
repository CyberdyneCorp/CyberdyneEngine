// SPDX-License-Identifier: MIT
// `integration.game_backend_graph`, continued: the Play debugger and hot reload over the graphs
// Play runs. Issues #84 (stages 2 and 3) and #29.
//
// What these cases hold, in order: a breakpoint stops the tick at its node and only for the entity
// it names; the work the break held runs on, in order, in the same tick; a debugged run that breaks
// and steps makes exactly the moves and cues an undebugged one does, tick for tick; a step visits
// nodes in the order the trace says they run; watches read the paused unit's pins and variables;
// and a reload keeps a running counter's count, while a refused one leaves the old program running.

#include <cy/graph/script_debug.h>

#include <string>
#include <vector>

#include "graph_scene.h"

namespace {

using namespace cy;
using game_backend::GraphBackend;
using game_backend::GraphInstanceStatus;
using game_backend::GraphPauseReason;
using game_backend::GraphStep;
using test_graph::allocator;
using test_graph::fixture;
using test_graph::kDt;
using test_graph::replaced;
using test_graph::Scene;

constexpr const char* kUnitGraph = "script_unit_command_v1.cyscript";
constexpr const char* kCounterGraph = "script_unit_counter_v1.cyscript";

/// Where every unit is after one tick, and every cue played so far.
struct Frame {
    std::vector<Vec3> positions;
    usize cues = 0;

    friend bool operator==(const Frame& a, const Frame& b) {
        if (a.cues != b.cues || a.positions.size() != b.positions.size()) {
            return false;
        }
        for (usize index = 0; index < a.positions.size(); ++index) {
            if (a.positions[index].x != b.positions[index].x ||
                a.positions[index].z != b.positions[index].z) {
                return false;
            }
        }
        return true;
    }
};

/// Three units ordered at tick zero: two arrive on the same tick, the third earlier.
struct Squad {
    Scene scene;
    std::vector<ecs::Entity> units;

    explicit Squad(const char* graph = kUnitGraph) {
        const u32 loaded = scene.load(GraphBackend::Bytecode, fixture(graph),
                                      graph == kUnitGraph ? "unit_command" : "unit_counter");
        for (const char* name : {"Alpha", "Bravo", "Charlie"}) {
            units.push_back(scene.unit(name));
            CY_REQUIRE(scene.graphs.attach(loaded, units.back()).has_value());
        }
    }

    [[nodiscard]] Frame frame() {
        Frame captured;
        for (const ecs::Entity unit : units) {
            captured.positions.push_back(scene.at(unit));
        }
        captured.cues = scene.graphs.cues().size();
        return captured;
    }
};

[[nodiscard]] bool debugger_available(Scene& scene) {
    const Status enabled = scene.graphs.set_debugging(true);
    if constexpr (!graph::script::kGraphDebuggerEnabled) {
        CY_REQUIRE(!enabled.has_value());
        CY_CHECK(enabled.error().code == ErrorCode::Unsupported);
        return false;
    }
    CY_REQUIRE(enabled.has_value());
    return true;
}

}  // namespace

CY_TEST_CASE("graph debugger: a breakpoint stops the tick at its node, for the entity it names") {
    Squad squad;
    if (!debugger_available(squad.scene)) {
        return;
    }
    auto& graphs = squad.scene.graphs;
    const Name graph = Name::intern("unit_command");
    // Node 4 is the move: a breakpoint there for Bravo alone.
    CY_REQUIRE(graphs.set_breakpoint(graph, 4, squad.units[1], true).has_value());

    CY_CHECK_EQ(squad.scene.order(squad.units[0], 6.0F, 8.0F), 1U);
    CY_CHECK(!graphs.paused());
    CY_CHECK(graphs.instance(0).moving);

    CY_CHECK_EQ(squad.scene.order(squad.units[1], 8.0F, 6.0F), 1U);
    CY_REQUIRE(graphs.paused());
    const game_backend::GraphPauseView& pause = graphs.pause_view();
    CY_CHECK(pause.reason == GraphPauseReason::Breakpoint);
    CY_CHECK_EQ(pause.instance, 1U);
    CY_CHECK(pause.entity == squad.units[1]);
    CY_CHECK_EQ(pause.node, 4U);
    CY_CHECK_EQ(pause.graph, graph);
    // Stopped BEFORE the move: Bravo has not been told to go anywhere.
    CY_CHECK(!graphs.instance(1).moving);

    // THE WHOLE SIMULATION WAITS: no tick and no other event while it is paused.
    const Vec3 alpha = squad.scene.at(squad.units[0]);
    CY_CHECK(!graphs.update(kDt).has_value());
    const f32 target[] = {1.0F, 0.0F, 1.0F};
    CY_CHECK(!graphs.raise(squad.units[2], Name::intern("unit.command"), Span<const f32>(target, 3))
                  .has_value());
    CY_CHECK_EQ(squad.scene.at(squad.units[0]).x, alpha.x);
    CY_CHECK_EQ(graphs.tick(), 0U);

    CY_REQUIRE(graphs.debug_continue().has_value());
    CY_CHECK(!graphs.paused());
    CY_CHECK(graphs.instance(1).moving);
    CY_CHECK(graphs.instance(1).status == GraphInstanceStatus::Waiting);

    // Charlie runs the same graph and the same node, and does not stop: the breakpoint is Bravo's.
    CY_CHECK_EQ(squad.scene.order(squad.units[2], 3.0F, 4.0F), 1U);
    CY_CHECK(!graphs.paused());

    // Cleared, nobody stops; set for every instance, everybody does.
    CY_REQUIRE(graphs.set_breakpoint(graph, 4, squad.units[1], false).has_value());
    CY_CHECK(graphs.breakpoints().empty());
    CY_CHECK_EQ(squad.scene.order(squad.units[1], 8.0F, 6.0F), 1U);
    CY_CHECK(!graphs.paused());
    CY_REQUIRE(graphs.set_breakpoint(graph, 4, ecs::Entity{}, true).has_value());
    CY_CHECK_EQ(squad.scene.order(squad.units[2], 3.0F, 4.0F), 1U);
    CY_CHECK(graphs.paused());
    CY_CHECK(graphs.pause_view().entity == squad.units[2]);
}

CY_TEST_CASE("graph debugger: the work a break held runs on, in order, in the same tick") {
    Squad squad;
    if (!debugger_available(squad.scene)) {
        return;
    }
    auto& graphs = squad.scene.graphs;
    // Node 6 plays the arrival cue. Alpha and Bravo walk ten metres and arrive on one tick.
    CY_REQUIRE(
        graphs.set_breakpoint(Name::intern("unit_command"), 6, ecs::Entity{}, true).has_value());
    CY_CHECK_EQ(squad.scene.order(squad.units[0], 6.0F, 8.0F), 1U);
    CY_CHECK_EQ(squad.scene.order(squad.units[1], 8.0F, 6.0F), 1U);
    while (!graphs.paused()) {
        CY_REQUIRE(graphs.update(kDt).has_value());
    }
    const u64 arrival = graphs.tick();
    CY_CHECK(graphs.pause_view().entity == squad.units[0]);
    CY_CHECK(graphs.cues().empty());

    // Alpha's cue plays, and the resume the break held — Bravo's — runs in the SAME tick and stops
    // at Bravo's cue.
    CY_REQUIRE(graphs.debug_continue().has_value());
    CY_REQUIRE(graphs.paused());
    CY_CHECK(graphs.pause_view().entity == squad.units[1]);
    CY_CHECK_EQ(graphs.tick(), arrival);
    CY_REQUIRE_EQ(graphs.cues().size(), 1U);

    CY_REQUIRE(graphs.debug_continue().has_value());
    CY_CHECK(!graphs.paused());
    CY_REQUIRE_EQ(graphs.cues().size(), 2U);
    CY_CHECK_EQ(graphs.cues()[0].tick, arrival);
    CY_CHECK_EQ(graphs.cues()[1].tick, arrival);
    CY_CHECK(graphs.cues()[0].entity == squad.units[0]);
    CY_CHECK(graphs.cues()[1].entity == squad.units[1]);
}

CY_TEST_CASE("graph debugger: a run that breaks and steps moves and sounds as one that does not") {
    Squad plain;
    Squad debugged;
    if (!debugger_available(debugged.scene)) {
        return;
    }
    auto& graphs = debugged.scene.graphs;
    const Name graph = Name::intern("unit_command");
    CY_REQUIRE(graphs.set_breakpoint(graph, 6, ecs::Entity{}, true).has_value());
    CY_REQUIRE(graphs.set_breakpoint(graph, 4, debugged.units[1], true).has_value());

    const f32 targets[3][2] = {{6.0F, 8.0F}, {8.0F, 6.0F}, {3.0F, 4.0F}};
    u32 breaks = 0;
    u32 action = 0;
    // Every break is answered differently in turn, so continue, step into and step over are each
    // exercised against the same simulation.
    const auto settle = [&]() {
        while (graphs.paused()) {
            ++breaks;
            const u32 choice = action++ % 3;
            const GraphStep step = choice == 1 ? GraphStep::Into : GraphStep::Over;
            const Status answered = choice == 0 ? graphs.debug_continue() : graphs.debug_step(step);
            CY_REQUIRE(answered.has_value());
        }
    };
    for (usize index = 0; index < 3; ++index) {
        CY_CHECK_EQ(plain.scene.order(plain.units[index], targets[index][0], targets[index][1]),
                    1U);
        CY_CHECK_EQ(
            debugged.scene.order(debugged.units[index], targets[index][0], targets[index][1]), 1U);
        settle();
    }
    for (u32 tick = 0; tick < 260; ++tick) {
        CY_REQUIRE(plain.scene.graphs.update(kDt).has_value());
        CY_REQUIRE(graphs.update(kDt).has_value());
        settle();
        DOCTEST_INFO("tick " << tick);
        CY_REQUIRE(plain.frame() == debugged.frame());
        CY_REQUIRE_EQ(plain.scene.graphs.tick(), graphs.tick());
    }
    CY_CHECK_GE(breaks, 4U);
    CY_REQUIRE_EQ(plain.scene.graphs.cues().size(), 3U);
    CY_REQUIRE_EQ(graphs.cues().size(), 3U);
    for (usize index = 0; index < 3; ++index) {
        CY_CHECK_EQ(plain.scene.graphs.cues()[index].tick, graphs.cues()[index].tick);
        CY_CHECK(plain.scene.graphs.cues()[index].entity.index() ==
                 graphs.cues()[index].entity.index());
    }
    CY_CHECK_EQ(plain.scene.audio.plays.size(), debugged.scene.audio.plays.size());
}

CY_TEST_CASE("graph debugger: a step visits nodes in the order the trace records them running") {
    Scene traced;
    const ecs::Entity runner = traced.unit("Runner");
    const u32 graph = traced.load(GraphBackend::Native, fixture(kUnitGraph));
    CY_REQUIRE(traced.graphs.attach(graph, runner).has_value());
    if (!debugger_available(traced)) {
        return;
    }
    // Undisturbed, with the debugger attached and nothing set: the trace is the execution order.
    CY_CHECK_EQ(traced.order(runner, 0.5F, 0.0F), 1U);
    for (u32 tick = 0; tick < 30; ++tick) {
        CY_REQUIRE(traced.graphs.update(kDt).has_value());
    }
    std::vector<graph::NodeKey> ran;
    ran.reserve(traced.graphs.trace_count());
    for (u32 index = 0; index < traced.graphs.trace_count(); ++index) {
        ran.push_back(traced.graphs.trace_entry(index).node);
    }
    CY_CHECK(ran == (std::vector<graph::NodeKey>{1, 2, 3, 4, 5, 6}));

    for (const GraphStep step : {GraphStep::Into, GraphStep::Over}) {
        Scene stepped;
        const ecs::Entity unit = stepped.unit("Runner");
        CY_REQUIRE(
            stepped.graphs.attach(stepped.load(GraphBackend::Native, fixture(kUnitGraph)), unit)
                .has_value());
        CY_REQUIRE(stepped.graphs.set_debugging(true).has_value());
        CY_REQUIRE(
            stepped.graphs.set_breakpoint(Name::intern("unit_command"), 1, unit, true).has_value());
        std::vector<graph::NodeKey> visited;
        CY_CHECK_EQ(stepped.order(unit, 0.5F, 0.0F), 1U);
        for (u32 tick = 0; tick < 30; ++tick) {
            while (stepped.graphs.paused()) {
                visited.push_back(stepped.graphs.pause_view().node);
                // A step leaves the breakpoint behind: clear it so only the step stops.
                CY_REQUIRE(
                    stepped.graphs.set_breakpoint(Name::intern("unit_command"), 1, unit, false)
                        .has_value());
                CY_REQUIRE(stepped.graphs.debug_step(step).has_value());
            }
            CY_REQUIRE(stepped.graphs.update(kDt).has_value());
        }
        // Into stops at every node, data nodes included; Over only where the chain executes. The
        // step across the wait stays armed and stops at the cue when the unit arrives.
        const std::vector<graph::NodeKey> expected =
            step == GraphStep::Into ? std::vector<graph::NodeKey>{1, 2, 3, 4, 5, 6}
                                    : std::vector<graph::NodeKey>{1, 4, 5, 6};
        CY_CHECK(visited == expected);
        CY_CHECK_EQ(stepped.graphs.cues().size(), 1U);
    }
}

CY_TEST_CASE("graph debugger: watches read the paused unit's pins and its variables") {
    Squad squad(kCounterGraph);
    if (!debugger_available(squad.scene)) {
        return;
    }
    auto& graphs = squad.scene.graphs;
    CY_CHECK_EQ(graphs.variable_count(0), 1U);
    CY_CHECK_EQ(squad.scene.order(squad.units[0], 1.0F, 0.0F), 1U);
    CY_CHECK_EQ(graphs.variable(0, 0).value.integer, 1);
    CY_CHECK_EQ(graphs.variable(0, 0).name, Name::intern("orders"));
    CY_CHECK_EQ(graphs.variable(0, 0).id, 7U);

    // Node 11 writes the count: stop there on Alpha's second order.
    CY_REQUIRE(
        graphs.set_breakpoint(Name::intern("unit_counter"), 11, squad.units[0], true).has_value());
    CY_CHECK_EQ(squad.scene.order(squad.units[0], 2.0F, 0.0F), 1U);
    CY_REQUIRE(graphs.paused());
    const graph::script::PinReading sum = graphs.watch_pin(0, 10, Name::intern("value"));
    const graph::script::PinReading read = graphs.watch_pin(0, 8, Name::intern("value"));
    CY_REQUIRE(sum.found);
    CY_REQUIRE(read.found);
    CY_CHECK_EQ(sum.value.integer, 2);
    CY_CHECK_EQ(read.value.integer, 1);
    CY_CHECK(sum.kind == graph::script::ValueKind::Int);
    // Not written yet: the write is the node it is paused before.
    CY_CHECK_EQ(graphs.variable(0, 0).value.integer, 1);
    // Bravo, the same graph, has its own count.
    CY_CHECK_EQ(graphs.variable(1, 0).value.integer, 0);

    CY_REQUIRE(graphs.debug_continue().has_value());
    CY_CHECK_EQ(graphs.variable(0, 0).value.integer, 2);
    CY_CHECK(!graphs.watch_pin(0, 10, Name::intern("nonsense")).found);
}

CY_TEST_CASE("graph debugger: Play without it runs the compiler's program and traces nothing") {
    Squad squad;
    auto& graphs = squad.scene.graphs;
    CY_CHECK(!graphs.debugging());
    CY_CHECK(graphs.program(0)->program().probes().empty());
    for (const graph::script::Instruction& instruction : graphs.program(0)->program().code()) {
        CY_CHECK(instruction.op != graph::script::ScriptOp::Probe);
    }
    CY_CHECK_EQ(squad.scene.order(squad.units[0], 1.0F, 0.0F), 1U);
    for (u32 tick = 0; tick < 40; ++tick) {
        CY_REQUIRE(graphs.update(kDt).has_value());
    }
    CY_CHECK_EQ(graphs.trace_count(), 0U);
    CY_CHECK_EQ(graphs.cues().size(), 1U);
    // A breakpoint set without the debugger attached stops nothing.
    CY_REQUIRE(
        graphs.set_breakpoint(Name::intern("unit_command"), 4, ecs::Entity{}, true).has_value());
    CY_CHECK_EQ(squad.scene.order(squad.units[1], 1.0F, 0.0F), 1U);
    CY_CHECK(!graphs.paused());
    CY_CHECK(!graphs.debug_pause().has_value());
}

CY_TEST_CASE("graph reload: a running counter keeps its count; a refused reload changes nothing") {
    for (const bool debugging : {false, true}) {
        Squad squad(kCounterGraph);
        auto& graphs = squad.scene.graphs;
        if (debugging && !debugger_available(squad.scene)) {
            return;
        }
        const Name name = Name::intern("unit_counter");
        const std::string source = fixture(kCounterGraph);
        for (u32 order = 0; order < 3; ++order) {
            CY_CHECK_EQ(squad.scene.order(squad.units[0], 1.0F, 0.0F), 1U);
        }
        CY_CHECK_EQ(graphs.variable(0, 0).value.integer, 3);
        CY_CHECK_EQ(graphs.generation(0), 1U);

        // Edited while the game runs: each order now counts two. Staged, then applied at the next
        // tick boundary to every instance, keeping each count.
        const std::string twice = replaced(source, R"(prop "value" : "int" = (0, 0, 0, 0, 1))",
                                           R"(prop "value" : "int" = (0, 0, 0, 0, 2))");
        graph::DiagnosticSink sink(allocator());
        const auto staged = graphs.reload(name, twice, sink);
        CY_REQUIRE(staged.has_value());
        CY_CHECK_EQ(*staged, 2U);
        CY_CHECK(graphs.reload_pending());
        CY_CHECK_EQ(graphs.generation(0), 1U);
        CY_REQUIRE(graphs.update(kDt).has_value());
        CY_CHECK(!graphs.reload_pending());
        CY_CHECK_EQ(graphs.generation(0), 2U);
        CY_CHECK_EQ(graphs.last_reload().instances, 3U);
        CY_CHECK_EQ(graphs.last_reload().kept, 3U);
        CY_CHECK_EQ(graphs.variable(0, 0).value.integer, 3);
        CY_CHECK_EQ(graphs.variable(1, 0).value.integer, 0);
        CY_CHECK_EQ(squad.scene.order(squad.units[0], 1.0F, 0.0F), 1U);
        CY_CHECK_EQ(graphs.variable(0, 0).value.integer, 5);

        // A type change is refused on the declaring node, and the running program is kept.
        const std::string retyped =
            replaced(twice, R"(prop "type" : "name" = "int")", R"(prop "type" : "name" = "float")");
        graph::DiagnosticSink refused(allocator());
        CY_CHECK(!graphs.reload(name, retyped, refused).has_value());
        CY_REQUIRE_EQ(refused.errors(), 1U);
        CY_CHECK_EQ(std::string(refused.entries()[0].code), "script.reload.type");
        CY_CHECK_EQ(refused.entries()[0].node, 7U);
        CY_CHECK(!graphs.reload_pending());
        CY_REQUIRE(graphs.update(kDt).has_value());
        CY_CHECK_EQ(graphs.generation(0), 2U);
        CY_CHECK_EQ(squad.scene.order(squad.units[0], 1.0F, 0.0F), 1U);
        CY_CHECK_EQ(graphs.variable(0, 0).value.integer, 7);

        // So is one that does not compile, named on its node.
        graph::DiagnosticSink broken(allocator());
        CY_CHECK(!graphs.reload(name, replaced(twice, "unit.move_to", "unit.mvoe_to"), broken)
                      .has_value());
        CY_CHECK_EQ(broken.entries()[0].node, 4U);
        CY_REQUIRE(graphs.update(kDt).has_value());
        CY_CHECK_EQ(graphs.generation(0), 2U);
        CY_CHECK(!graphs.reload(Name::intern("no_such_graph"), twice, broken).has_value());
    }
}

CY_TEST_CASE("graph reload: a unit walking to its target keeps waiting across a reload") {
    Squad squad;
    auto& graphs = squad.scene.graphs;
    CY_CHECK_EQ(squad.scene.order(squad.units[0], 6.0F, 8.0F), 1U);
    for (u32 tick = 0; tick < 20; ++tick) {
        CY_REQUIRE(graphs.update(kDt).has_value());
    }
    CY_REQUIRE(graphs.instance(0).status == GraphInstanceStatus::Waiting);

    // The edit swaps the order's axes for later orders; the wait in progress is the same node.
    std::string edited = replaced(fixture(kUnitGraph), R"(link 2 "value" -> 4 "arg0")",
                                  R"(link 2 "value" -> 4 "arg1")");
    edited = replaced(edited, R"(link 3 "value" -> 4 "arg1")", R"(link 3 "value" -> 4 "arg0")");
    graph::DiagnosticSink sink(allocator());
    CY_REQUIRE(graphs.reload(Name::intern("unit_command"), edited, sink).has_value());
    for (u32 tick = 0; tick < 260; ++tick) {
        CY_REQUIRE(graphs.update(kDt).has_value());
    }
    CY_CHECK_EQ(graphs.last_reload().waits_kept, 1U);
    CY_CHECK_EQ(graphs.last_reload().waits_dropped, 0U);
    CY_CHECK_EQ(squad.scene.at(squad.units[0]).x, 6.0F);
    CY_CHECK_EQ(squad.scene.at(squad.units[0]).z, 8.0F);
    CY_REQUIRE_EQ(graphs.cues().size(), 1U);
    CY_CHECK(graphs.instance(0).status == GraphInstanceStatus::Idle);
}

CY_TEST_CASE("graph reload: a reload staged while paused waits for the held tick to finish") {
    Squad squad(kCounterGraph);
    if (!debugger_available(squad.scene)) {
        return;
    }
    auto& graphs = squad.scene.graphs;
    CY_REQUIRE(
        graphs.set_breakpoint(Name::intern("unit_counter"), 11, ecs::Entity{}, true).has_value());
    CY_CHECK_EQ(squad.scene.order(squad.units[0], 1.0F, 0.0F), 1U);
    CY_REQUIRE(graphs.paused());
    graph::DiagnosticSink sink(allocator());
    CY_REQUIRE(
        graphs.reload(Name::intern("unit_counter"), fixture(kCounterGraph), sink).has_value());
    CY_CHECK(graphs.reload_pending());
    CY_CHECK_EQ(graphs.generation(0), 1U);
    CY_REQUIRE(graphs.debug_continue().has_value());
    CY_CHECK_EQ(graphs.variable(0, 0).value.integer, 1);
    CY_REQUIRE(graphs.update(kDt).has_value());
    CY_CHECK_EQ(graphs.generation(0), 2U);
    CY_CHECK_EQ(graphs.variable(0, 0).value.integer, 1);
}
