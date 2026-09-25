#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/registry.h>
#include <cy/gameplay/play/session.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/servers/physics/reference/server.h>
#include <cy/test/test.h>
#include <cy_reflect_generated_scene.h>

#include "script_runtime.h"

namespace {

namespace ser = cy::scene::serialization;

constexpr std::string_view kWorld = R"(cyworld 1
type 1 runtime "Transform"
  field 1 quat "rotation" ""
  field 2 vec3 "translation" ""
  field 3 vec3 "scale" ""
type 2 runtime "ScriptBehaviour"
  field 4 text "class" ""
node 0 - "test" "SpinCube"
  component 1
    field 1 0 0 0 1
    field 2 2 0 0
    field 3 1 1 1
  component 2
    field 4 "SpinCube"
)";

cy::Status tick_script(void* user, cy::gameplay::PlaySession& play, cy::f32 dt) noexcept {
    return static_cast<cy::sample::editor_window::ScriptRuntime*>(user)->tick(play, dt);
}

}  // namespace

CY_TEST_CASE("a Swift scene behaviour rotates during Play and restores on Stop") {
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::World);
    cy::reflect::TypeRegistry registry;
    CY_REQUIRE(cy::reflect::register_scene_types(registry).has_value());
    ser::AuthoringSchema schema(allocator);
    CY_REQUIRE(ser::build_authoring_schema(registry, schema).has_value());
    ser::World world(allocator);
    CY_REQUIRE(ser::read_world(kWorld, "worlds/spinning-cube.cyworld", world).has_value());
    CY_REQUIRE(ser::resolve_against(world, schema).has_value());

    const cy::Expected<cy::physics::PhysicsServer*, cy::Error> created =
        cy::physics::reference::create_server(allocator);
    CY_REQUIRE(created.has_value());
    cy::physics::PhysicsServer* physics = *created;
    CY_REQUIRE(physics->initialize().has_value());

    cy::gameplay::PlaySession play(allocator, world);
    cy::sample::editor_window::ScriptRuntime scripts(allocator, "", CY_SPIN_MODULE);
    cy::gameplay::PlayConfiguration configuration;
    configuration.physics = physics;
    configuration.schema = &schema;
    configuration.gameplay_tick = &tick_script;
    configuration.gameplay_user = &scripts;
    CY_REQUIRE(play.enter(configuration).has_value());
    CY_REQUIRE(scripts.start(play, world).has_value());
    CY_CHECK_EQ(scripts.count(), 1U);
    for (int step = 0; step < 60; ++step) {
        CY_REQUIRE(play.tick().has_value());
    }
    cy::Transform spinning;
    CY_REQUIRE(ser::transform_of(world, world.nodes()[0], spinning));
    CY_CHECK_GT(spinning.rotation.z, 0.35F);
    CY_CHECK_LT(spinning.rotation.z, 0.41F);

    CY_REQUIRE(play.pause().has_value());
    CY_REQUIRE(play.tick().has_value());
    cy::Transform paused;
    CY_REQUIRE(ser::transform_of(world, world.nodes()[0], paused));
    CY_CHECK_EQ(paused.rotation.z, spinning.rotation.z);

    scripts.stop();
    CY_REQUIRE(play.stop().has_value());
    cy::Transform restored;
    CY_REQUIRE(ser::transform_of(world, world.nodes()[0], restored));
    CY_CHECK_EQ(restored.rotation.z, 0.0F);
    CY_CHECK_EQ(restored.rotation.w, 1.0F);
    CY_CHECK(play.report().restored_exactly);
    physics->shutdown();
    cy::physics::reference::destroy_server(physics, allocator);
}
