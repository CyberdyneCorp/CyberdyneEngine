// SPDX-License-Identifier: MIT
// `smoke.editor_graph_equivalence` — issue #29's acceptance for visual scripting, as far as a test
// can hold it: the unit-command graph the editor authors, run in Play by the engine's compiled
// program, does what its Swift twin does.
//
// Two Play sessions over two one-node worlds. In the first the node carries a `ScriptGraph` naming
// the editor's graph (`src/editor_backend/tests/data/script_unit_command_v1.cyscript`, the file the
// editor's MCP suite writes byte for byte) and receives the `unit.command` event; in the second it
// carries `ScriptBehaviour` naming `CommandedUnit`, the Swift behaviour in
// `project/game/CommandedUnit.swift`, with the same target exported. Both are run through the
// hosted runtime's own glue (`GraphRuntime`, `ScriptRuntime`), and both reach the same audio
// backend, through the C ABI for Swift and directly for the graph. The case requires the same
// position on every tick, the arrival on the same tick, and one `unit.arrived` cue each, on the
// same tick and at the same place.

#include <cy/abi/cy_abi.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/registry.h>
#include <cy/gameplay/play/session.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/servers/physics/reference/server.h>
#include <cy/test/test.h>
#include <cy_reflect_generated_scene.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "graph_runtime.h"
#include "script_runtime.h"

namespace {

namespace ser = cy::scene::serialization;
using cy::f32;
using cy::u32;

constexpr std::string_view kGraphWorld = R"(cyworld 1
type 1 runtime "Transform"
  field 1 quat "rotation" ""
  field 2 vec3 "translation" ""
  field 3 vec3 "scale" ""
type 2 runtime "ScriptGraph"
  field 4 text "graph" ""
node 0 - "test" "GraphUnit"
  component 1
    field 1 0 0 0 1
    field 2 0 0 0
    field 3 1 1 1
  component 2
    field 4 "script_unit_command_v1.cyscript"
)";

constexpr std::string_view kSwiftWorld = R"(cyworld 1
type 1 runtime "Transform"
  field 1 quat "rotation" ""
  field 2 vec3 "translation" ""
  field 3 vec3 "scale" ""
type 2 runtime "ScriptBehaviour"
  field 4 text "class" ""
  field 5 float "targetX" ""
  field 6 float "targetZ" ""
  field 7 float "speed" ""
node 0 - "test" "SwiftUnit"
  component 1
    field 1 0 0 0 1
    field 2 0 0 0
    field 3 1 1 1
  component 2
    field 4 "CommandedUnit"
    field 5 6
    field 6 8
    field 7 3
)";

constexpr u32 kTicks = 260;

/// The one cue both units may play, and every play with the tick it happened on.
class RecordingAudio final : public cy::abi::game::AudioBackend {
public:
    struct Played {
        u32 tick;
        CyAudioCue cue;
        f32 x;
        f32 y;
        f32 z;
    };

    CyResult find_cue(const char* name, CyAudioCue& out_cue) noexcept override {
        out_cue = 11;
        return std::string_view(name) == "unit.arrived" ? CY_RESULT_OK : CY_RESULT_NOT_FOUND;
    }
    CyResult play(const CyAudioPlay& request, CyAudioVoice& out_voice) noexcept override {
        plays.push_back(Played{tick, request.cue, request.position[0], request.position[1],
                               request.position[2]});
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
    std::vector<Played> plays;
};

struct Trace {
    std::vector<cy::Vec3> positions;
    std::vector<RecordingAudio::Played> plays;
};

/// Everything a Play needs beside its world: the schema it is resolved against and a solver.
struct Stage {
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::World);
    cy::reflect::TypeRegistry registry;
    ser::AuthoringSchema schema{allocator};
    cy::physics::PhysicsServer* physics = nullptr;

    Stage() {
        CY_REQUIRE(cy::reflect::register_scene_types(registry).has_value());
        CY_REQUIRE(ser::build_authoring_schema(registry, schema).has_value());
        const auto created = cy::physics::reference::create_server(allocator);
        CY_REQUIRE(created.has_value());
        physics = *created;
        CY_REQUIRE(physics->initialize().has_value());
    }

    ~Stage() {
        physics->shutdown();
        cy::physics::reference::destroy_server(physics, allocator);
    }

    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;

    void read(std::string_view text, ser::World& world) const {
        CY_REQUIRE(ser::read_world(text, "worlds/unit.cyworld", world).has_value());
        CY_REQUIRE(ser::resolve_against(world, schema).has_value());
    }
};

cy::Status tick_graphs(void* user, cy::gameplay::PlaySession& /*play*/, f32 dt) noexcept {
    return static_cast<cy::sample::editor_window::GraphRuntime*>(user)->tick(dt);
}

cy::Status tick_swift(void* user, cy::gameplay::PlaySession& play, f32 dt) noexcept {
    return static_cast<cy::sample::editor_window::ScriptRuntime*>(user)->tick(play, dt);
}

void record(cy::gameplay::PlaySession& play, ser::World& world, RecordingAudio& audio,
            Trace& trace) {
    for (u32 tick = 1; tick <= kTicks; ++tick) {
        audio.tick = tick;
        CY_REQUIRE(play.tick().has_value());
        cy::Transform placed;
        CY_REQUIRE(ser::transform_of(world, world.nodes()[0], placed));
        trace.positions.push_back(placed.translation);
    }
    trace.plays = audio.plays;
}

Trace run_graph(Stage& stage) {
    ser::World world(stage.allocator);
    stage.read(kGraphWorld, world);
    RecordingAudio audio;
    cy::sample::editor_window::GraphRuntime graphs(stage.allocator, CY_SCRIPT_FIXTURE_DIR);
    cy::gameplay::PlaySession play(stage.allocator, world);
    cy::gameplay::PlayConfiguration configuration;
    configuration.physics = stage.physics;
    configuration.schema = &stage.schema;
    configuration.gameplay_tick = &tick_graphs;
    configuration.gameplay_user = &graphs;
    CY_REQUIRE(play.enter(configuration).has_value());
    const cy::Status started = graphs.start(play, world, &audio);
    CY_TEST_INFO(graphs.problem());
    CY_REQUIRE(started.has_value());
    CY_REQUIRE_EQ(graphs.count(), 1U);

    // The order, as the editor's `script.event.raise` delivers it: before the first tick.
    const f32 target[] = {6.0F, 0.0F, 8.0F};
    const cy::ecs::Entity unit = graphs.entity_for(world.nodes()[0].identity);
    const auto raised =
        graphs.raise(unit, cy::Name::intern("unit.command"), cy::Span<const f32>(target, 3));
    CY_REQUIRE(raised.has_value());
    CY_REQUIRE_EQ(*raised, 1U);

    Trace trace;
    record(play, world, audio, trace);
    graphs.stop();
    CY_REQUIRE(play.stop().has_value());
    return trace;
}

Trace run_swift(Stage& stage) {
    ser::World world(stage.allocator);
    stage.read(kSwiftWorld, world);
    RecordingAudio audio;
    cy::sample::editor_window::ScriptRuntime scripts(stage.allocator, "", CY_TWIN_MODULE);
    scripts.bind_audio(&audio);
    cy::gameplay::PlaySession play(stage.allocator, world);
    cy::gameplay::PlayConfiguration configuration;
    configuration.physics = stage.physics;
    configuration.schema = &stage.schema;
    configuration.gameplay_tick = &tick_swift;
    configuration.gameplay_user = &scripts;
    CY_REQUIRE(play.enter(configuration).has_value());
    CY_REQUIRE(scripts.start(play, world).has_value());
    CY_REQUIRE_EQ(scripts.count(), 1U);

    Trace trace;
    record(play, world, audio, trace);
    scripts.stop();
    CY_REQUIRE(play.stop().has_value());
    return trace;
}

}  // namespace

CY_TEST_CASE(
    "the editor's unit-command graph moves and sounds as its Swift twin does, tick for "
    "tick") {
    Stage stage;
    const Trace graph = run_graph(stage);
    const Trace swift = run_swift(stage);

    CY_REQUIRE_EQ(graph.positions.size(), swift.positions.size());
    u32 graph_arrived = 0;
    u32 swift_arrived = 0;
    for (u32 index = 0; index < graph.positions.size(); ++index) {
        const cy::Vec3 left = graph.positions[index];
        const cy::Vec3 right = swift.positions[index];
        CY_TEST_INFO("tick " << (index + 1));
        CY_CHECK_EQ(left.x, right.x);
        CY_CHECK_EQ(left.y, right.y);
        CY_CHECK_EQ(left.z, right.z);
        if (graph_arrived == 0 && left.x == 6.0F && left.z == 8.0F) {
            graph_arrived = index + 1;
        }
        if (swift_arrived == 0 && right.x == 6.0F && right.z == 8.0F) {
            swift_arrived = index + 1;
        }
    }
    // Ten metres at three metres a second, in sixtieths: the two hundredth tick, give or take the
    // rounding of two hundred additions.
    CY_CHECK_GE(graph_arrived, 199U);
    CY_CHECK_LE(graph_arrived, 201U);
    CY_CHECK_EQ(graph_arrived, swift_arrived);

    CY_REQUIRE_EQ(graph.plays.size(), 1U);
    CY_REQUIRE_EQ(swift.plays.size(), 1U);
    CY_CHECK_EQ(graph.plays[0].tick, graph_arrived);
    CY_CHECK_EQ(graph.plays[0].tick, swift.plays[0].tick);
    CY_CHECK_EQ(graph.plays[0].cue, swift.plays[0].cue);
    CY_CHECK_EQ(graph.plays[0].x, swift.plays[0].x);
    CY_CHECK_EQ(graph.plays[0].y, swift.plays[0].y);
    CY_CHECK_EQ(graph.plays[0].z, swift.plays[0].z);
}

CY_TEST_CASE("the sample project's unit-command graph is the editor's, byte for byte") {
    const auto read = [](const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        CY_REQUIRE(file.good());
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    };
    CY_CHECK(read(std::string(CY_EDITOR_PROJECT_DIR) + "/game/scripts/unit_command.cyscript") ==
             read(std::string(CY_SCRIPT_FIXTURE_DIR) + "/script_unit_command_v1.cyscript"));
}
