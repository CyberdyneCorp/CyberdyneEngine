// SPDX-License-Identifier: MIT
#include "scene_vfx_runtime.h"

#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/registry.h>
#include <cy/test/test.h>
#include <cy_reflect_generated_scene.h>

#include <string>

using namespace cy;
using namespace cy::sample::editor_window;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

constexpr std::string_view kScene = R"(cyworld 1
type 1 runtime "Transform"
  field 1 quat "rotation" ""
  field 2 vec3 "translation" ""
  field 3 vec3 "scale" ""
type 2 runtime "cy::vfx::Effect"
  field 4 text "asset" ""
  field 5 bool "enabled" ""
  field 6 float "system.speed.float" ""
node 0 - "fx" "Quiet"
  component 1
    field 1 0 0 0 1
    field 2 1 2 3
    field 3 1 1 1
  component 2
    field 4 "effects/issue15_two_emitters.cyvfxdoc"
    field 5 true
    field 6 2
node 1 - "fx" "Loud"
  component 1
    field 1 0 0 0 1
    field 2 4 5 6
    field 3 1 1 1
  component 2
    field 4 "effects/issue15_two_emitters.cyvfxdoc"
    field 5 true
    field 6 4
)";

}  // namespace

CY_TEST_CASE("the hosted editor cooks saved scene VFX assets and steps two independent effects") {
    scene::serialization::World scene(allocator());
    CY_REQUIRE(scene::serialization::read_world(kScene, "worlds/vfx.cyworld", scene));
    reflect::TypeRegistry registry;
    CY_REQUIRE(reflect::register_scene_types(registry));
    scene::serialization::AuthoringSchema schema(allocator());
    CY_REQUIRE(scene::serialization::build_authoring_schema(registry, schema));
    CY_REQUIRE(scene::serialization::resolve_against(scene, schema));

    SceneVfxRuntime runtime(allocator(), CY_SAMPLE_PROJECT);
    CY_REQUIRE(runtime.initialize());
    CY_REQUIRE(runtime.load(scene));
    CY_CHECK_EQ(runtime.instances(), 2U);
    CY_REQUIRE(runtime.world() != nullptr);
    for (u32 frame = 0; frame < 3; ++frame) {
        CY_REQUIRE(runtime.step(scene, 1.0F / 30.0F));
    }
    const auto instances = runtime.world()->instances();
    CY_REQUIRE_EQ(instances.size(), 2U);
    CY_CHECK_GT(instances[0].live_particles, 0U);
    CY_CHECK_EQ(instances[1].live_particles, instances[0].live_particles * 2U);
    CY_CHECK_EQ(instances[0].position.x, 1.0F);
    CY_CHECK_EQ(instances[1].position.x, 4.0F);

    const vfx::EffectHandle first = instances[0].handle;
    const vfx::EffectHandle second = instances[1].handle;
    const u32 first_population = instances[0].live_particles;
    std::string changed(kScene);
    const usize override = changed.find("field 6 2\n");
    CY_REQUIRE(override != std::string::npos);
    changed.replace(override, std::string("field 6 2\n").size(), "field 6 6\n");
    scene::serialization::World edited(allocator());
    CY_REQUIRE(scene::serialization::read_world(changed, "worlds/vfx.cyworld", edited));
    CY_REQUIRE(scene::serialization::resolve_against(edited, schema));
    CY_REQUIRE(runtime.load(edited));
    CY_CHECK_EQ(runtime.world()->instances()[0].handle, first);
    CY_CHECK_EQ(runtime.world()->instances()[1].handle, second);
    CY_CHECK_EQ(runtime.world()->instances()[0].live_particles, first_population);
    CY_REQUIRE(runtime.step(edited, 1.0F / 30.0F));
    CY_CHECK_GT(runtime.world()->instances()[0].live_particles, first_population);

    auto first_speed =
        runtime.get_parameter(edited.nodes()[0].identity, Name{}, Name::intern("speed"));
    auto second_speed =
        runtime.get_parameter(edited.nodes()[1].identity, Name{}, Name::intern("speed"));
    CY_REQUIRE(first_speed.has_value());
    CY_REQUIRE(second_speed.has_value());
    CY_CHECK_EQ(first_speed->lanes[0], 6.0F);
    CY_CHECK_EQ(second_speed->lanes[0], 4.0F);
    const f32 live_speed[] = {8.0F};
    CY_REQUIRE(runtime.set_parameter(edited.nodes()[0].identity, Name{}, Name::intern("speed"),
                                     live_speed));
    CY_CHECK_EQ(
        runtime.get_parameter(edited.nodes()[0].identity, Name{}, Name::intern("speed"))->lanes[0],
        8.0F);
    CY_CHECK_EQ(
        runtime.get_parameter(edited.nodes()[1].identity, Name{}, Name::intern("speed"))->lanes[0],
        4.0F);

    Transform moved = Transform::identity();
    moved.translation = Vec3{9.0F, 8.0F, 7.0F};
    CY_REQUIRE(scene::serialization::set_transform(edited, edited.nodes()[0], moved));
    CY_REQUIRE(runtime.load(edited));
    CY_CHECK_EQ(
        runtime.get_parameter(edited.nodes()[0].identity, Name{}, Name::intern("speed"))->lanes[0],
        8.0F);
    CY_REQUIRE(runtime.step(edited, 1.0F / 30.0F));
    CY_CHECK_EQ(runtime.world()->instances()[0].position.x, 9.0F);
}
