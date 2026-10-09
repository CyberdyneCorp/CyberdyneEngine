// SPDX-License-Identifier: MIT
// `integration.editor_window_play_animation` — #112's gaps: an event the author placed on the
// Animation panel's timeline reaches the game in Play, at the time it was placed.
//
// The whole path a project takes, with the hosted runtime's own glue and no stand-ins: a character
// FBX imported by the real importer (`animation_character_fixture.h`) and its records written
// where the importer writes them (`<project>/.cy/cooked/<id>.cyasset`); a graph with two events
// baked for it by `AnimationRigBaker` reading those files through `ProjectAnimationAssets`; the
// bake's files written where the editor writes them (`.cy/cooked/animation/hero/`); and Play's
// `PlayAnimation` loading the rig through the asset system and `AnimationLibrary`, as a game
// would, and animating an entity of a running `PlaySession` through the ABI's animation backend —
// the calls a Swift behaviour's `Animator.attach` and `Animation.events` make.
// `smoke.editor_animation_events` drives the same rig from a Swift behaviour.

#include <cy/abi/cy_abi.h>
#include <cy/abi/game/services.h>
#include <cy/core/assets/cooked.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/registry.h>
#include <cy/editor/animation_rig.h>
#include <cy/gameplay/play/session.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/servers/physics/reference/server.h>
#include <cy/test/test.h>
#include <cy_reflect_generated_scene.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "play_animation.h"
#include "play_animation_fixture.h"

namespace {

namespace ser = cy::scene::serialization;
using namespace cy;
using sample::editor_window::testing::allocator;
using sample::editor_window::testing::baked_project;
using sample::editor_window::testing::write_file;

constexpr std::string_view kWorld = R"(cyworld 1
type 1 runtime "Transform"
  field 1 quat "rotation" ""
  field 2 vec3 "translation" ""
  field 3 vec3 "scale" ""
node 0 - "test" "Hero"
  component 1
    field 1 0 0 0 1
    field 2 0 0 0
    field 3 1 1 1
)";

/// A running Play over a one-node world, its solver and schema kept alive beside it.
struct Playing {
    Playing() : schema(allocator()), world(allocator()) {
        CY_REQUIRE(reflect::register_scene_types(registry).has_value());
        CY_REQUIRE(ser::build_authoring_schema(registry, schema).has_value());
        CY_REQUIRE(ser::read_world(kWorld, "worlds/hero.cyworld", world).has_value());
        CY_REQUIRE(ser::resolve_against(world, schema).has_value());
        Expected<physics::PhysicsServer*, Error> created =
            physics::reference::create_server(allocator());
        CY_REQUIRE(created.has_value());
        physics = *created;
        CY_REQUIRE(physics->initialize().has_value());
        play = std::make_unique<gameplay::PlaySession>(allocator(), world);
        gameplay::PlayConfiguration configuration;
        configuration.physics = physics;
        configuration.schema = &schema;
        CY_REQUIRE(play->enter(configuration).has_value());
    }

    ~Playing() {
        (void)play->stop();
        play.reset();
        physics->shutdown();
        physics::reference::destroy_server(physics, allocator());
    }

    Playing(const Playing&) = delete;
    Playing& operator=(const Playing&) = delete;

    [[nodiscard]] CyEntity hero() const {
        return play->entity_for(world.nodes()[0].identity).bits();
    }

    reflect::TypeRegistry registry;
    ser::AuthoringSchema schema;
    ser::World world;
    physics::PhysicsServer* physics = nullptr;
    std::unique_ptr<gameplay::PlaySession> play;
};

CyResult attach(abi::game::AnimationBackend& backend, CyEntity entity, const char* rig) {
    CyAnimatorDesc desc{};
    desc.struct_size = static_cast<u32>(sizeof(CyAnimatorDesc));
    desc.rig = rig;
    return backend.attach(entity, desc);
}

}  // namespace

CY_TEST_CASE("Play loads a baked rig, and the timeline's events arrive on the ticks they fall in") {
    const std::filesystem::path project = baked_project("play-animation-events");
    Playing playing;
    sample::editor_window::PlayAnimation animation(allocator(), project.string().c_str());
    CY_REQUIRE(animation.start(*playing.play).has_value());
    CY_CHECK(animation.problems().empty());
    CY_REQUIRE_EQ(animation.rigs().size(), 1U);
    CY_CHECK_EQ(animation.rigs()[0], "hero");
    abi::game::AnimationBackend* backend = animation.backend();
    CY_REQUIRE(backend != nullptr);
    CY_REQUIRE_EQ(attach(*backend, playing.hero(), "hero"), CY_RESULT_OK);

    std::vector<std::pair<u64, u32>> fired;
    for (u32 tick = 1; tick <= 60; ++tick) {
        CY_REQUIRE(animation.tick(1.0F / 60.0F).has_value());
        for (const CyAnimationEvent& event : backend->events()) {
            CY_CHECK_EQ(event.entity, playing.hero());
            fired.emplace_back(event.name, tick);
        }
    }
    CY_REQUIRE_EQ(fired.size(), 2U);
    CY_CHECK_EQ(fired[0].first, abi::game::name_hash("footstep"));
    CY_CHECK_EQ(fired[0].second, 16U);
    CY_CHECK_EQ(fired[1].first, abi::game::name_hash("land"));
    CY_CHECK_EQ(fired[1].second, 46U);
    // The pose is the imported clip's: the spine has turned.
    CyPose spine{};
    CY_REQUIRE_EQ(backend->joint_pose(playing.hero(), "Spine", spine), CY_RESULT_OK);
    CY_CHECK_GT(spine.rotation[2] * spine.rotation[2], 0.0001F);
    animation.stop();
    CY_CHECK(animation.backend() == nullptr);
}

CY_TEST_CASE("Play skips a rig that does not load, names it, and plays the rest") {
    const std::filesystem::path project = baked_project("play-animation-stale");
    // A rig whose character was deleted after it was baked.
    const std::string stale =
        "cyrig 1\nrig \"ghost\"\nmodel \"gone.fbx\"\nskeleton "
        "\"0123456789abcdef0123456789abcdef\"\n"
        "mesh \"\"\nprogram \"program.cyasset\"\n";
    write_file(project / ".cy" / "cooked" / "animation" / "ghost" / "rig.cyrig", stale.data(),
               stale.size());
    // And one whose manifest reaches out of its directory for a record.
    std::string escaping;
    {
        std::ifstream manifest(project / ".cy" / "cooked" / "animation" / "hero" / "rig.cyrig");
        std::ostringstream text;
        text << manifest.rdbuf();
        escaping = text.str();
    }
    const usize program = escaping.find("program \"program.cyasset\"");
    CY_REQUIRE(program != std::string::npos);
    escaping.replace(program, 25, "program \"../hero/program.cyasset\"");
    escaping.replace(escaping.find("rig \"hero\""), 10, "rig \"thief\"");
    write_file(project / ".cy" / "cooked" / "animation" / "thief" / "rig.cyrig", escaping.data(),
               escaping.size());
    Playing playing;
    sample::editor_window::PlayAnimation animation(allocator(), project.string().c_str());
    CY_REQUIRE(animation.start(*playing.play).has_value());
    CY_REQUIRE_EQ(animation.rigs().size(), 1U);
    CY_CHECK_EQ(animation.rigs()[0], "hero");
    CY_REQUIRE_EQ(animation.problems().size(), 2U);
    CY_CHECK_NE(animation.problems()[0].find("ghost"), std::string::npos);
    CY_CHECK_NE(animation.problems()[0].find("skeleton"), std::string::npos);
    CY_CHECK_NE(animation.problems()[1].find("thief"), std::string::npos);
    CY_CHECK_NE(animation.problems()[1].find("outside"), std::string::npos);
}

CY_TEST_CASE("Play with no project loads no rig, not even the working directory's") {
    const std::filesystem::path project = baked_project("play-animation-cwd");
    const std::filesystem::path before = std::filesystem::current_path();
    std::filesystem::current_path(project);
    Playing playing;
    sample::editor_window::PlayAnimation animation(allocator(), "");
    const Status started = animation.start(*playing.play);
    std::filesystem::current_path(before);
    CY_REQUIRE(started.has_value());
    CY_CHECK(animation.rigs().empty());
    CY_CHECK(animation.problems().empty());
}

CY_TEST_CASE("Play with nothing baked animates nothing and refuses an unknown rig by name") {
    const std::filesystem::path project =
        std::filesystem::path(CY_TEST_BINARY_DIR) / "play-animation-empty";
    std::filesystem::remove_all(project);
    std::filesystem::create_directories(project);
    Playing playing;
    sample::editor_window::PlayAnimation animation(allocator(), project.string().c_str());
    CY_REQUIRE(animation.start(*playing.play).has_value());
    CY_CHECK(animation.rigs().empty());
    CY_REQUIRE(animation.backend() != nullptr);
    CY_CHECK_EQ(attach(*animation.backend(), playing.hero(), "hero"), CY_RESULT_NOT_FOUND);
}
