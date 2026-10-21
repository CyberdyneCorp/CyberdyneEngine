// SPDX-License-Identifier: MIT
// A project's Swift game in the editor's Play tunes the GI the viewport is lit by. Issue #35, the
// live half.
//
// The runtime binds ABI 1.9 for Play over the preview's own system (`AuthoredIllumination::
// bind_play`, what `main.cpp`'s Play start calls): every object the GI holds by its Play entity,
// every material by its authored name. The room of `tests/data/gi_test_room.cyworld`, its GI at
// MEDIUM quality, gets one more node, a `ScriptBehaviour` running bindings/swift's illumination
// fixture (`LightProbe.swift`), which in its first frame asks for HIGH quality and places a volume
// and removes it. Asserted:
//
//   the game's settings are the viewport's   the preview's system runs at the game's quality, the
//                                            same system the device frame resolves — nothing
//                                            rebuilt
//   the control                              Play without the binding leaves the document's quality
//   Stop                                     the document's GI applied again: medium, its one
//                                            volume

#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/registry.h>
#include <cy/gameplay/play/session.h>
#include <cy/scene/serialization/authoring_schema.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/servers/physics/reference/server.h>
#include <cy/test/test.h>
#include <cy_reflect_generated_scene.h>

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "gi_runtime.h"
#include "script_runtime.h"

using namespace cy;
using namespace cy::sample::editor_window;
namespace gi = cy::rendering::gi;
namespace ser = cy::scene::serialization;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// The room's primitives copied into a temporary project, and its world with the probe's node.
struct Project {
    std::filesystem::path root;
    std::string world;

    Project() {
        static u32 counter = 0;
        root = std::filesystem::temp_directory_path() /
               ("cy-play-gi-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(root);
        for (const char* name : {"gi_box.cyprim", "gi_floor.cyprim"}) {
            std::filesystem::copy_file(std::filesystem::path(CY_GI_TEST_DATA) / name, root / name);
        }
        Array<u8> bytes(allocator());
        const std::string path = std::string(CY_GI_TEST_DATA) + "/gi_test_room.cyworld";
        CY_REQUIRE(assets::fs::read_whole(path.c_str(), bytes).has_value());
        world.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        // The document's GI at medium quality, so the game's "high" is a change.
        const auto replace = [this](std::string_view from, std::string_view to) {
            const usize at = world.find(from);
            CY_REQUIRE(at != std::string::npos);
            world.replace(at, from.size(), to);
        };
        replace("field 21 \"high\"", "field 21 \"medium\"");
        // The cart's rigid body has no collider, which Play refuses; it stands still here.
        replace("  component 8\n    field 70 40\n", "");
        replace("node 0 - ",
                "type 9 runtime \"ScriptBehaviour\"\n  field 90 text \"class\" \"\"\nnode 0 - ");
        world +=
            "node 8 - \"gi\" \"Probe\"\n  component 1\n    field 1 0 0 0 1\n"
            "    field 2 2 1 2\n    field 3 1 1 1\n  component 9\n    field 90 \"LightProbe\"\n";
    }
    ~Project() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;
};

Status tick_scripts(void* user, gameplay::PlaySession& play, f32 dt) noexcept {
    auto* scripts = static_cast<ScriptRuntime*>(user);
    if (Status ticked = scripts->tick(play, dt); !ticked) {
        return ticked;
    }
    // The runtime's order: the fixed step, then the frame that reads it.
    scripts->frame(dt);
    return ok();
}

/// One Play session of `ticks` over the project's world, ABI 1.9 bound when `bind` says so; the
/// preview's quality while it played, and the volumes the adapter held.
struct Played {
    gi::GiQuality quality = gi::GiQuality::Count;
    gi::GiQuality after_stop = gi::GiQuality::Count;
    u64 rebuilds_before = 0;
    u64 rebuilds_after = 0;
    usize volumes_after_stop = 99;
    bool bound = false;
};

[[nodiscard]] Played play(const Project& project, bool bind, u32 ticks) {
    Played out;
    reflect::TypeRegistry registry;
    CY_REQUIRE(reflect::register_scene_types(registry).has_value());
    ser::AuthoringSchema schema(allocator());
    CY_REQUIRE(ser::build_authoring_schema(registry, schema).has_value());
    ser::World world(allocator());
    CY_REQUIRE(ser::read_world(project.world, "worlds/gi.cyworld", world).has_value());
    CY_REQUIRE(ser::resolve_against(world, schema).has_value());

    AuthoredIllumination gi_source(allocator(), project.root.string());
    gi_source.bind(&world);
    AuthoredGi lit;
    CY_REQUIRE(gi_source.current(lit));
    CY_REQUIRE(lit.system->runtime_settings().quality == gi::GiQuality::Medium);
    out.rebuilds_before = gi_source.preview()->rebuilds();

    const Expected<physics::PhysicsServer*, Error> created =
        physics::reference::create_server(allocator());
    CY_REQUIRE(created.has_value());
    physics::PhysicsServer* physics = *created;
    CY_REQUIRE(physics->initialize().has_value());
    {
        gameplay::PlaySession session(allocator(), world);
        ScriptRuntime scripts(allocator(), "", CY_SWIFT_ILLUMINATION_MODULE);
        gameplay::PlayConfiguration configuration;
        configuration.physics = physics;
        configuration.schema = &schema;
        configuration.gameplay_tick = &tick_scripts;
        configuration.gameplay_user = &scripts;
        CY_REQUIRE(session.enter(configuration).has_value());
        // PLAY'S START, as main.cpp's: ABI 1.9 before the scripts, so `onCreate` could use it.
        std::unique_ptr<game_backend::IlluminationAdapter> adapter;
        if (bind) {
            adapter = gi_source.bind_play(session);
            CY_REQUIRE(adapter != nullptr);
            scripts.bind_illumination(adapter.get());
            out.bound = true;
        }
        CY_REQUIRE(scripts.start(session, world).has_value());
        CY_REQUIRE_EQ(scripts.count(), 1U);
        for (u32 tick = 0; tick < ticks; ++tick) {
            const Status ticked = session.tick();
            if (!ticked) {
                std::fprintf(stderr, "play tick %u: %s\n", tick, ticked.error().message);
            }
            CY_REQUIRE(ticked.has_value());
        }
        out.quality = gi_source.preview()->system().runtime_settings().quality;
        // PLAY'S STOP: the binding goes, and the document's GI is applied again.
        scripts.stop();
        scripts.bind_illumination(nullptr);
        adapter.reset();
        CY_REQUIRE(session.stop().has_value());
    }
    gi_source.world_changed();
    CY_REQUIRE(gi_source.current(lit));
    out.after_stop = lit.system->runtime_settings().quality;
    out.volumes_after_stop = lit.system->volumes().size();
    out.rebuilds_after = gi_source.preview()->rebuilds();
    physics->shutdown();
    physics::reference::destroy_server(physics, allocator());
    return out;
}

}  // namespace

CY_TEST_CASE(
    "a project's Swift game in Play tunes the GI the viewport is lit by, and Stop ends it") {
    const Project project;
    const Played bound = play(project, true, 4);
    // The game asked for high quality in its first frame: the preview's system — the one the
    // device frame resolves — runs at it, nothing rebuilt.
    CY_CHECK(bound.quality == gi::GiQuality::High);
    CY_CHECK_EQ(bound.rebuilds_after, bound.rebuilds_before);
    // Stop: the document's medium again, and the document's one volume (the crate's).
    CY_CHECK(bound.after_stop == gi::GiQuality::Medium);
    CY_CHECK_EQ(bound.volumes_after_stop, 1U);

    // THE CONTROL: the same Play with nothing bound leaves the document's GI as it was.
    const Played unbound = play(project, false, 4);
    CY_CHECK(unbound.quality == gi::GiQuality::Medium);
}
