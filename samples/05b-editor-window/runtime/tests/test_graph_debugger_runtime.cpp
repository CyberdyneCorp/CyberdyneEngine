// SPDX-License-Identifier: MIT
// `integration.editor_window_graph_debugger` — the Play debugger and hot reload through the hosted
// runtime's own glue (`GraphRuntime`), over a real `PlaySession`. Issues #84 and #29.
//
// The question these cases answer is the one the engine's suite cannot: WHAT PAUSES. A graph that
// breaks must stop the whole simulation — the session's clock, its physics, the other systems —
// and not merely the graph, or the world would run on under a paused script and the run would no
// longer be the run without the debugger. So: a break pauses the session and tells the host to
// pause its audio; ticking a held session advances nothing; a continue finishes the held tick and
// resumes the session; and the whole run, placement for placement and cue for cue, is the run with
// no debugger at all. Then a reload through the runtime keeps a running count.

#include <cy/abi/cy_abi.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/registry.h>
#include <cy/gameplay/play/session.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/servers/physics/reference/server.h>
#include <cy/test/test.h>
#include <cy_reflect_generated_scene.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "graph_runtime.h"

namespace {

namespace ser = cy::scene::serialization;
using cy::f32;
using cy::u32;
using cy::u64;
using cy::sample::editor_window::GraphRuntime;

constexpr std::string_view kWorld = R"(cyworld 1
type 1 runtime "Transform"
  field 1 quat "rotation" ""
  field 2 vec3 "translation" ""
  field 3 vec3 "scale" ""
type 2 runtime "ScriptGraph"
  field 4 text "graph" ""
node 0 - "test" "Counter"
  component 1
    field 1 0 0 0 1
    field 2 0 0 0
    field 3 1 1 1
  component 2
    field 4 "script_unit_counter_v1.cyscript"
)";

constexpr const char* kReference = "script_unit_counter_v1.cyscript";

class RecordingAudio final : public cy::abi::game::AudioBackend {
public:
    CyResult find_cue(const char* name, CyAudioCue& out_cue) noexcept override {
        out_cue = 11;
        return std::string_view(name) == "unit.arrived" ? CY_RESULT_OK : CY_RESULT_NOT_FOUND;
    }
    CyResult play(const CyAudioPlay& /*request*/, CyAudioVoice& out_voice) noexcept override {
        plays.push_back(tick);
        out_voice = plays.size();
        return CY_RESULT_OK;
    }
    CyResult stop(CyAudioVoice /*voice*/, f32 /*fade*/) noexcept override { return CY_RESULT_OK; }
    bool playing(CyAudioVoice /*voice*/) const noexcept override { return false; }
    CyResult find_bus(const char* /*name*/, CyAudioBus& /*bus*/) noexcept override {
        return CY_RESULT_NOT_FOUND;
    }
    CyResult set_bus_volume(CyAudioBus /*bus*/, f32 /*volume*/, f32 /*fade*/) noexcept override {
        return CY_RESULT_OK;
    }

    u32 tick = 0;
    std::vector<u32> plays;
};

cy::Status tick_graphs(void* user, cy::gameplay::PlaySession& /*play*/, f32 dt) noexcept {
    return static_cast<GraphRuntime*>(user)->tick(dt);
}

/// One Play of the counter world: a session, the graph runtime, and what the host was told.
struct Session {
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::World);
    cy::reflect::TypeRegistry registry;
    ser::AuthoringSchema schema{allocator};
    cy::physics::PhysicsServer* physics = nullptr;
    ser::World world{allocator};
    RecordingAudio audio;
    GraphRuntime graphs{allocator, CY_SCRIPT_FIXTURE_DIR};
    cy::gameplay::PlaySession play{allocator, world};
    std::vector<bool> holds;
    cy::ecs::Entity unit;

    Session() {
        CY_REQUIRE(cy::reflect::register_scene_types(registry).has_value());
        CY_REQUIRE(ser::build_authoring_schema(registry, schema).has_value());
        const auto created = cy::physics::reference::create_server(allocator);
        CY_REQUIRE(created.has_value());
        physics = *created;
        CY_REQUIRE(physics->initialize().has_value());
        CY_REQUIRE(ser::read_world(kWorld, "worlds/counter.cyworld", world).has_value());
        CY_REQUIRE(ser::resolve_against(world, schema).has_value());
        cy::gameplay::PlayConfiguration configuration;
        configuration.physics = physics;
        configuration.schema = &schema;
        configuration.gameplay_tick = &tick_graphs;
        configuration.gameplay_user = &graphs;
        CY_REQUIRE(play.enter(configuration).has_value());
        const cy::Status started = graphs.start(play, world, &audio);
        DOCTEST_INFO(graphs.problem());
        CY_REQUIRE(started.has_value());
        graphs.set_hold_listener(
            [](void* user, bool held) noexcept {
                static_cast<Session*>(user)->holds.push_back(held);
            },
            this);
        unit = graphs.entity_for(world.nodes()[0].identity);
    }

    ~Session() {
        graphs.stop();
        (void)play.stop();
        physics->shutdown();
        cy::physics::reference::destroy_server(physics, allocator);
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void order(f32 x, f32 z) {
        const f32 target[] = {x, 0.0F, z};
        const auto raised =
            graphs.raise(unit, cy::Name::intern("unit.command"), cy::Span<const f32>(target, 3));
        CY_REQUIRE(raised.has_value());
    }

    [[nodiscard]] cy::Vec3 placed() {
        cy::Transform transform;
        CY_REQUIRE(ser::transform_of(world, world.nodes()[0], transform));
        return transform.translation;
    }
};

std::string fixture(const char* name) {
    std::ifstream file(std::string(CY_SCRIPT_FIXTURE_DIR) + "/" + name, std::ios::binary);
    CY_REQUIRE(file.good());
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

}  // namespace

CY_TEST_CASE("a graph breakpoint pauses the whole Play session until the debugger releases it") {
    if constexpr (!cy::graph::script::kGraphDebuggerEnabled) {
        Session session;
        // Compiled out: Play runs the plain programs and the runtime says there is no debugger.
        CY_CHECK(!session.graphs.behaviours()->debugging());
        CY_CHECK(!session.graphs.debug(cy::editor::ScriptDebugAction::Pause).has_value());
        return;
    }
    Session plain;
    Session debugged;
    CY_REQUIRE(debugged.graphs.behaviours()->debugging());
    // Node 6 plays the arrival cue. Play names a graph after its file's stem.
    CY_REQUIRE(
        debugged.graphs
            .set_breakpoint(cy::Name::intern("script_unit_counter_v1"), 6, cy::ecs::Entity{}, true)
            .has_value());
    plain.order(6.0F, 8.0F);
    debugged.order(6.0F, 8.0F);

    u32 held_at = 0;
    for (u32 tick = 1; tick <= 260; ++tick) {
        plain.audio.tick = tick;
        debugged.audio.tick = tick;
        CY_REQUIRE(plain.play.tick().has_value());
        CY_REQUIRE(debugged.play.tick().has_value());
        if (debugged.graphs.held()) {
            held_at = tick;
            // WHAT PAUSES IS THE SIMULATION: the session is paused, the host was told, and ticking
            // it advances neither its clock nor the world.
            CY_CHECK(debugged.play.state() == cy::gameplay::PlayState::Paused);
            CY_CHECK(debugged.holds == std::vector<bool>{true});
            const u64 ticks = debugged.play.report().ticks;
            const cy::Vec3 there = debugged.placed();
            for (u32 idle = 0; idle < 10; ++idle) {
                CY_REQUIRE(debugged.play.tick().has_value());
            }
            CY_CHECK_EQ(debugged.play.report().ticks, ticks);
            CY_CHECK_EQ(debugged.placed().x, there.x);
            CY_CHECK(debugged.audio.plays.empty());
            // A Play resume would start a tick with this one unfinished; the debugger continues it.
            CY_REQUIRE(debugged.graphs.debug(cy::editor::ScriptDebugAction::Continue).has_value());
            CY_CHECK(!debugged.graphs.held());
            CY_CHECK(debugged.play.state() == cy::gameplay::PlayState::Playing);
            CY_CHECK(debugged.holds == (std::vector<bool>{true, false}));
        }
        DOCTEST_INFO("tick " << tick);
        CY_REQUIRE_EQ(plain.placed().x, debugged.placed().x);
        CY_REQUIRE_EQ(plain.placed().z, debugged.placed().z);
        CY_REQUIRE_EQ(plain.play.report().ticks, debugged.play.report().ticks);
    }
    CY_CHECK_GE(held_at, 199U);
    CY_CHECK_LE(held_at, 201U);
    CY_REQUIRE_EQ(plain.audio.plays.size(), 1U);
    CY_CHECK(plain.audio.plays == debugged.audio.plays);
}

CY_TEST_CASE("a graph saved during Play is swapped in at the next tick and keeps its count") {
    Session session;
    for (u32 order = 0; order < 3; ++order) {
        session.order(1.0F, 0.0F);
    }
    const cy::game_backend::GraphBehaviours& graphs = *session.graphs.behaviours();
    CY_CHECK_EQ(graphs.variable(0, 0).value.integer, 3);
    std::string source = fixture(kReference);
    const std::string one = R"(prop "value" : "int" = (0, 0, 0, 0, 1))";
    source.replace(source.find(one), one.size(), R"(prop "value" : "int" = (0, 0, 0, 0, 5))");
    cy::graph::DiagnosticSink sink(session.allocator);
    const auto staged = session.graphs.reload(kReference, source, sink);
    CY_REQUIRE(staged.has_value());
    CY_CHECK_EQ(*staged, 2U);
    CY_REQUIRE(session.play.tick().has_value());
    CY_CHECK_EQ(graphs.generation(0), 2U);
    session.order(1.0F, 0.0F);
    CY_CHECK_EQ(graphs.variable(0, 0).value.integer, 8);

    cy::graph::DiagnosticSink elsewhere(session.allocator);
    CY_CHECK(!session.graphs.reload("game/scripts/other.cyscript", source, elsewhere).has_value());
}
