// SPDX-License-Identifier: MIT
#pragma once
// The scene `integration.game_backend_graph`'s cases share: a world, a tree, a recording audio
// backend and `GraphBehaviours` started on them, and the editor's graphs read from the one fixture
// directory both sides compare against.

#include <cy/abi/cy_abi.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/game_backend/graph_behaviours.h>
#include <cy/scene/node.h>
#include <cy/scene/tree.h>
#include <cy/test/test.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace test_graph {

using namespace cy;
using game_backend::GraphBackend;
using game_backend::GraphBehaviours;
using game_backend::GraphInstanceStatus;

inline Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Scripting);
}

inline std::string fixture(const char* name) {
    std::ifstream file(std::string(CY_SCRIPT_FIXTURE_DIR) + "/" + name, std::ios::binary);
    CY_REQUIRE(file.good());
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

inline std::string replaced(std::string text, const std::string& from, const std::string& to) {
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

    u32 load(GraphBackend backend, const std::string& source, const char* name = "unit_command") {
        graph::DiagnosticSink sink(allocator());
        const auto loaded = graphs.load(Name::intern(name), source, backend, sink);
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

inline constexpr f32 kDt = 1.0F / 60.0F;

}  // namespace test_graph
