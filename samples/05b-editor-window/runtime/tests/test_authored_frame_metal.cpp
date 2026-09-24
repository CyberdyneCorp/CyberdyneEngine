// SPDX-License-Identifier: MIT
#include <cy/backends/rhi-metal/backend.h>
#include <cy/backends/rhi/backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/registry.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/test/test.h>
#include <cy_reflect_generated_scene.h>

#include "authored_frame.h"

#include <string_view>

using namespace cy;
using namespace cy::sample::editor_window;
namespace ser = cy::scene::serialization;
namespace first_light = cy::sample::first_light;

namespace {

constexpr std::string_view kEmpty = "cyworld 1\n";
constexpr std::string_view kSphere = R"(cyworld 1
type 1 runtime "Transform"
  field 1 quat "rotation" ""
  field 2 vec3 "translation" ""
  field 3 vec3 "scale" ""
type 2 runtime "MeshRenderer"
  field 4 text "mesh" ""
node 0 - "test" "Sphere"
  component 1
    field 1 0 0 0 1
    field 2 0 0 0
    field 3 1 1 1
  component 2
    field 4 "content/beauty/meshes/block.cyprim"
)";
constexpr std::string_view kTransformed = R"(cyworld 1
type 1 runtime "Transform"
  field 1 quat "rotation" ""
  field 2 vec3 "translation" ""
  field 3 vec3 "scale" ""
type 2 runtime "MeshRenderer"
  field 4 text "mesh" ""
node 0 - "test" "Sphere"
  component 1
    field 1 0 0.7071068 0 0.7071068
    field 2 2 0 0
    field 3 2 1 0.5
  component 2
    field 4 "content/beauty/meshes/block.cyprim"
)";
constexpr std::string_view kParented = R"(cyworld 1
type 1 runtime "Transform"
  field 1 quat "rotation" ""
  field 2 vec3 "translation" ""
  field 3 vec3 "scale" ""
type 2 runtime "MeshRenderer"
  field 4 text "mesh" ""
node 0 - "test" "Parent"
  component 1
    field 1 0 0 0 1
    field 2 2 0 0
    field 3 1 1 1
node 1 0 "test" "Child"
  component 1
    field 1 0 0 0 1
    field 2 1 0 0
    field 3 1 1 1
  component 2
    field 4 "content/beauty/meshes/block.cyprim"
)";
constexpr std::string_view kLit = R"(cyworld 1
type 1 runtime "Transform"
  field 1 quat "rotation" ""
  field 2 vec3 "translation" ""
  field 3 vec3 "scale" ""
type 2 runtime "MeshRenderer"
  field 4 text "mesh" ""
type 3 runtime "LightSource"
  field 5 int "kind" ""
  field 6 float "intensity" ""
  field 7 float "range" ""
  field 8 bool "enabled" ""
node 0 - "test" "Sphere"
  component 1
    field 1 0 0 0 1
    field 2 0 0 0
    field 3 1 1 1
  component 2
    field 4 "content/beauty/meshes/block.cyprim"
node 1 - "test" "Point Light"
  component 1
    field 1 0 0 0 1
    field 2 0 2 2
    field 3 1 1 1
  component 3
    field 5 1
    field 6 5000
    field 7 10
    field 8 true
)";
constexpr std::string_view kCamera = R"(cyworld 1
type 1 runtime "Transform"
  field 1 quat "rotation" ""
  field 2 vec3 "translation" ""
  field 3 vec3 "scale" ""
type 2 runtime "Camera"
  field 4 float "projection.fov_y" ""
  field 5 bool "enabled" ""
node 0 - "test" "Parent"
  component 1
    field 1 0 0 0 1
    field 2 2 0 0
    field 3 1 1 1
node 1 0 "test" "Camera"
  component 1
    field 1 0 0 0 1
    field 2 1 0 0
    field 3 1 1 1
  component 2
    field 4 1.1
    field 5 true
)";

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Gpu);
}

first_light::Camera camera() {
    first_light::Camera view;
    view.position[0] = 0.0;
    view.position[1] = 1.0;
    view.position[2] = 5.0;
    view.forward = normalize(Vec3{0.0F, -0.12F, -1.0F});
    view.up = Vec3{0.0F, 1.0F, 0.0F};
    view.fov_y_radians = 0.9F;
    view.near_plane = 0.1F;
    return view;
}

}  // namespace

CY_TEST_CASE("authored Metal frame renders a mesh and publishes its transformed bounds") {
    (void)rhi::metal::register_metal_backend();
    rhi::DeviceDescription description;
    description.application_name = "smoke.editor_authored_frame_metal";
    description.enable_validation = true;
    rhi::BackendSelection selection;
    auto device =
        rhi::create_device(allocator(), rhi::metal::kMetalBackendName, description, selection);
    CY_REQUIRE(device.has_value());

    {
        AuthoredFrame frame(allocator(), **device);
        CY_REQUIRE(frame.initialize(192, 128, CY_TEST_PROJECT));
        ser::World empty(allocator());
        ser::World sphere(allocator());
        ser::World transformed(allocator());
        ser::World parented(allocator());
        ser::World lit(allocator());
        ser::World authored_camera(allocator());
        CY_REQUIRE(ser::read_world(kEmpty, "worlds/test.cyworld", empty).has_value());
        CY_REQUIRE(ser::read_world(kSphere, "worlds/test.cyworld", sphere).has_value());
        CY_REQUIRE(ser::read_world(kTransformed, "worlds/test.cyworld", transformed).has_value());
        CY_REQUIRE(ser::read_world(kParented, "worlds/test.cyworld", parented).has_value());
        CY_REQUIRE(ser::read_world(kLit, "worlds/test.cyworld", lit).has_value());
        CY_REQUIRE(ser::read_world(kCamera, "worlds/test.cyworld", authored_camera).has_value());
        reflect::TypeRegistry registry;
        CY_REQUIRE(reflect::register_scene_types(registry));
        ser::AuthoringSchema schema(allocator());
        CY_REQUIRE(ser::build_authoring_schema(registry, schema));
        CY_REQUIRE(ser::resolve_against(sphere, schema).has_value());
        CY_REQUIRE(ser::resolve_against(transformed, schema).has_value());
        CY_REQUIRE(ser::resolve_against(parented, schema).has_value());
        CY_REQUIRE(ser::resolve_against(lit, schema).has_value());
        CY_REQUIRE(ser::resolve_against(authored_camera, schema).has_value());
        const first_light::Camera view = camera();
        CY_REQUIRE(frame.render(empty, view));
        Array<u32> blank(allocator());
        CY_REQUIRE(blank.append(frame.pixels()));
        CY_REQUIRE(frame.render(sphere, view));
        usize changed = 0;
        for (usize pixel = 0; pixel < blank.size(); ++pixel) {
            changed += static_cast<usize>(blank[pixel] != frame.pixels()[pixel]);
        }
        CY_CHECK(changed > 100);

        Array<render::GpuInstance> instances(allocator());
        Array<render::DrawItem> draws(allocator());
        CY_REQUIRE(frame.publish(view, instances, draws));
        CY_REQUIRE_EQ(instances.size(), 1U);
        CY_REQUIRE_EQ(draws.size(), 1U);
        CY_CHECK_EQ(instances[0].stable_id(), sphere.nodes()[0].identity);
        CY_CHECK(instances[0].bounds_radius > 0.6F);
        const first_light::Camera framed = frame.framing(view);
        CY_CHECK(framed.position[2] < 3.0);
        Array<u32> before_transform(allocator());
        CY_REQUIRE(before_transform.append(frame.pixels()));
        CY_REQUIRE(frame.render(transformed, view));
        usize moved_pixels = 0;
        for (usize pixel = 0; pixel < before_transform.size(); ++pixel) {
            moved_pixels += static_cast<usize>(before_transform[pixel] != frame.pixels()[pixel]);
        }
        CY_CHECK(moved_pixels > 100);
        CY_REQUIRE(frame.publish(view, instances, draws));
        CY_REQUIRE_EQ(instances.size(), 1U);
        CY_CHECK(instances[0].bounds_center[0] > 1.0F);
        CY_CHECK(instances[0].bounds_radius > 1.0F);
        CY_REQUIRE(frame.render(parented, view));
        CY_REQUIRE(frame.publish(view, instances, draws));
        CY_REQUIRE_EQ(instances.size(), 1U);
        CY_CHECK_EQ(instances[0].stable_id(), parented.nodes()[1].identity);
        CY_CHECK(instances[0].bounds_center[0] > 2.0F);
        Vec3 parent_pivot;
        CY_CHECK(frame.pivot_for(parented.nodes()[0].identity, parent_pivot));
        CY_CHECK(parent_pivot.x > 1.9F);

        first_light::Camera game_camera;
        CY_CHECK(frame.scene_camera(authored_camera, 0, game_camera));
        CY_CHECK(game_camera.position[0] > 2.9);
        CY_CHECK(game_camera.fov_y_radians > 1.0F);
        CY_CHECK(!frame.scene_camera(empty, 0, game_camera));

        CY_REQUIRE(frame.render(sphere, view, false));
        Array<u32> unlit(allocator());
        CY_REQUIRE(unlit.append(frame.pixels()));
        CY_REQUIRE(frame.render(lit, view, false));
        usize lighting_changed = 0;
        for (usize pixel = 0; pixel < unlit.size(); ++pixel) {
            lighting_changed += static_cast<usize>(unlit[pixel] != frame.pixels()[pixel]);
        }
        CY_CHECK(lighting_changed > 50);
    }
    rhi::destroy_device(allocator(), *device);
}
