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

namespace {

using namespace cy;
using game_backend::GraphBackend;
using game_backend::GraphBehaviours;
using game_backend::GraphInstanceStatus;

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Scripting);
}

std::string fixture(const char* name) {
    std::ifstream file(std::string(CY_SCRIPT_FIXTURE_DIR) + "/" + name, std::ios::binary);
    CY_REQUIRE(file.good());
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const usize at = text.find(from);
    CY_REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}

/// The audio a graph reaches: one cue, `unit.arrived`, and a record of every play.
class RecordingAudio final : public abi::game::AudioBackend {
public:
    struct Played {
        CyAudioCue cue;
        f32 x;
        f32 z;
        bool spatial;
    };

    CyResult find_cue(const char* name, CyAudioCue& out_cue) noexcept override {
        if (std::string(name) != "unit.arrived") {
            return CY_RESULT_NOT_FOUND;
        }
        out_cue = kArrived;
        return CY_RESULT_OK;
    }
    CyResult play(const CyAudioPlay& play, CyAudioVoice& out_voice) noexcept override {
        plays.push_back(Played{play.cue, play.position[0], play.position[2],
                               (play.flags & CY_AUDIO_PLAY_SPATIAL) != 0});
        out_voice = plays.size();
        return CY_RESULT_OK;
    }
    CyResult stop(CyAudioVoice /*voice*/, f32 /*fade*/) noexcept override { return CY_RESULT_OK; }
    bool playing(CyAudioVoice /*voice*/) const noexcept override { return false; }
    CyResult find_bus(const char* /*name*/, CyAudioBus& /*out_bus*/) noexcept override {
        return CY_RESULT_NOT_FOUND;
    }
    CyResult set_bus_volume(CyAudioBus /*bus*/, f32 /*volume*/, f32 /*fade*/) noexcept override {
        return CY_RESULT_OK;
    }

    static constexpr CyAudioCue kArrived = 7;
    std::vector<Played> plays;
};

/// A world, a tree, and units placed at the origin.
struct Scene {
    ecs::World world{system_allocator(MemoryDomain::World)};
    scene::SceneTree tree{world};
    RecordingAudio audio;
    GraphBehaviours graphs{allocator()};

    Scene() {
        CY_REQUIRE(world.initialize().has_value());
        CY_REQUIRE(tree.initialize().has_value());
        graphs.start(tree, &audio);
    }

    ecs::Entity unit(const char* name) {
        const auto node = tree.create_node(Name::intern(name), tree.root());
        CY_REQUIRE(node.has_value());
        return node->entity();
    }

    Vec3 at(ecs::Entity entity) { return tree.node(entity).local_transform().translation; }

    u32 load(GraphBackend backend, const std::string& source) {
        graph::DiagnosticSink sink(allocator());
        const auto loaded = graphs.load(Name::intern("unit_command"), source, backend, sink);
        CY_REQUIRE(loaded.has_value());
        CY_CHECK_EQ(sink.errors(), 0U);
        CY_CHECK_EQ(sink.warnings(), 0U);
        return *loaded;
    }

    u32 order(ecs::Entity entity, f32 x, f32 z) {
        const f32 arguments[] = {x, 0.0F, z};
        const auto started =
            graphs.raise(entity, Name::intern("unit.command"), Span<const f32>(arguments, 3));
        CY_REQUIRE(started.has_value());
        return *started;
    }
};

constexpr f32 kDt = 1.0F / 60.0F;

}  // namespace

CY_TEST_CASE("graph behaviours: an ordered unit moves to the target and plays its cue on arrival") {
    Scene scene;
    const ecs::Entity unit = scene.unit("Tank");
    const u32 graph = scene.load(GraphBackend::Bytecode, fixture("script_unit_command_v1.cyscript"));
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
    const u32 graph = scene.load(GraphBackend::Bytecode, fixture("script_unit_command_v1.cyscript"));
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
    const u32 graph = scene.load(GraphBackend::Bytecode, fixture("script_unit_command_v1.cyscript"));
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
    const u32 graph = scene.load(GraphBackend::Bytecode, fixture("script_unit_command_v1.cyscript"));
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
    const auto loaded = scene.graphs.load(Name::intern("unit_command"), source,
                                          GraphBackend::Bytecode, sink);
    CY_CHECK(!loaded.has_value());
    CY_REQUIRE_EQ(sink.entries().size(), 1U);
    CY_CHECK_EQ(std::string(sink.entries()[0].code), "script.cue.unknown");
    CY_CHECK_EQ(sink.entries()[0].node, 6U);
    CY_CHECK_EQ(sink.entries()[0].detail, Name::intern("unit.vanished"));
}

CY_TEST_CASE("graph behaviours: a function the engine does not declare is an error on its node") {
    Scene scene;
    const std::string source = replaced(fixture("script_unit_command_v1.cyscript"),
                                        "unit.move_to", "unit.mvoe_to");
    graph::DiagnosticSink sink(allocator());
    const auto loaded = scene.graphs.load(Name::intern("unit_command"), source,
                                          GraphBackend::Bytecode, sink);
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
