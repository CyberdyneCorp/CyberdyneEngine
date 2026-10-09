// SPDX-License-Identifier: MIT
// `smoke.editor_animation_events` — #112's gaps, end to end in Swift: an event the author placed on
// the Animation panel's timeline is delivered to a Swift behaviour's frame callback in Play, on the
// tick its time falls in.
//
// The project is `play_animation_fixture.h`'s: the hero imported by the real FBX importer and a
// graph with `footstep@0.26` and `land@0.76` baked for it. Play runs the hosted runtime's own glue
// in its own order — `ScriptRuntime::tick` (the behaviours' fixed step), `PlayAnimation::tick` (the
// rig advanced, the frame's events snapshotted), `ScriptRuntime::frame` (`onUpdate`) — over the
// project's `AnimatedHero` (`project/game/AnimatedHero.swift`), which attaches to the baked rig by
// name and writes what `Animation.events(for:)` gave it into its node's translation. Smoke because
// it builds and loads a Swift module.

#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/registry.h>
#include <cy/gameplay/play/session.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/servers/physics/reference/server.h>
#include <cy/test/test.h>
#include <cy_reflect_generated_scene.h>

#include "play_animation.h"
#include "play_animation_fixture.h"
#include "script_runtime.h"

namespace {

namespace ser = cy::scene::serialization;
using namespace cy;
using sample::editor_window::testing::allocator;

constexpr std::string_view kWorld = R"(cyworld 1
type 1 runtime "Transform"
  field 1 quat "rotation" ""
  field 2 vec3 "translation" ""
  field 3 vec3 "scale" ""
type 2 runtime "ScriptBehaviour"
  field 4 text "class" ""
node 0 - "test" "Hero"
  component 1
    field 1 0 0 0 1
    field 2 0 0 0
    field 3 1 1 1
  component 2
    field 4 "AnimatedHero"
)";

struct Glue {
    sample::editor_window::ScriptRuntime* scripts = nullptr;
    sample::editor_window::PlayAnimation* animation = nullptr;
};

/// What the hosted runtime's `tick_gameplay` does, in its order.
Status tick(void* user, gameplay::PlaySession& play, f32 dt) noexcept {
    Glue& glue = *static_cast<Glue*>(user);
    if (Status scripted = glue.scripts->tick(play, dt); !scripted) {
        return scripted;
    }
    if (Status animated = glue.animation->tick(dt); !animated) {
        return animated;
    }
    glue.scripts->frame(dt);
    return ok();
}

}  // namespace

CY_TEST_CASE("a Swift behaviour receives the timeline's events in onUpdate on their ticks") {
    const std::filesystem::path project =
        sample::editor_window::testing::baked_project("swift-animation-events");
    reflect::TypeRegistry registry;
    CY_REQUIRE(reflect::register_scene_types(registry).has_value());
    ser::AuthoringSchema schema(allocator());
    CY_REQUIRE(ser::build_authoring_schema(registry, schema).has_value());
    ser::World world(allocator());
    CY_REQUIRE(ser::read_world(kWorld, "worlds/hero.cyworld", world).has_value());
    CY_REQUIRE(ser::resolve_against(world, schema).has_value());
    Expected<physics::PhysicsServer*, Error> created =
        physics::reference::create_server(allocator());
    CY_REQUIRE(created.has_value());
    physics::PhysicsServer* physics = *created;
    CY_REQUIRE(physics->initialize().has_value());

    gameplay::PlaySession play(allocator(), world);
    sample::editor_window::ScriptRuntime scripts(allocator(), project.string().c_str(),
                                                 CY_TWIN_MODULE);
    sample::editor_window::PlayAnimation animation(allocator(), project.string().c_str());
    Glue glue{&scripts, &animation};
    gameplay::PlayConfiguration configuration;
    configuration.physics = physics;
    configuration.schema = &schema;
    configuration.gameplay_tick = &tick;
    configuration.gameplay_user = &glue;
    CY_REQUIRE(play.enter(configuration).has_value());
    CY_REQUIRE(animation.start(play).has_value());
    CY_REQUIRE_EQ(animation.rigs().size(), 1U);
    scripts.bind_animation(animation.backend());
    const Status started = scripts.start(play, world);
    CY_REQUIRE_MESSAGE(started.has_value(), (started ? "" : started.error().message));
    CY_REQUIRE_EQ(scripts.count(), 1U);
    for (int step = 0; step < 60; ++step) {
        CY_REQUIRE(play.tick().has_value());
    }
    Transform seen;
    CY_REQUIRE(ser::transform_of(world, world.nodes()[0], seen));
    // One footstep in the first second, on tick 16 (0.26 s); the landing on tick 46 (0.76 s).
    CY_CHECK_EQ(seen.translation.x, 1.0F);
    CY_CHECK_EQ(seen.translation.y, 16.0F);
    CY_CHECK_EQ(seen.translation.z, 46.0F);

    scripts.stop();
    animation.stop();
    CY_REQUIRE(play.stop().has_value());
    physics->shutdown();
    physics::reference::destroy_server(physics, allocator());
}
