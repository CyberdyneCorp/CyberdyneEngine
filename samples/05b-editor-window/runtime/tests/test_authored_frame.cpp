// SPDX-License-Identifier: MIT
// The editor's authored frame on the native device this host publishes from: Metal on Apple,
// Vulkan on Linux. One file so both backends answer to the same pixel assertions.
#include <cy/backends/rhi/backend.h>
#if defined(__APPLE__)
#    include <cy/backends/rhi-metal/backend.h>
#else
#    include <cy/backends/rhi/vulkan/vulkan_backend.h>
#endif
#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/registry.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/test/test.h>
#include <cy_reflect_generated_scene.h>

#include "authored_frame.h"

#include <string>
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
constexpr std::string_view kGraphMaterial = R"(cyworld 1
type 1 runtime "Transform"
  field 1 quat "rotation" ""
  field 2 vec3 "translation" ""
  field 3 vec3 "scale" ""
type 2 runtime "MeshRenderer"
  field 4 text "mesh" ""
  field 5 text "material" ""
type 3 runtime "Material: copper_clay"
  field 6 vec3 "albedo" ""
node 0 - "test" "Graph Block"
  component 1
    field 1 0 0 0 1
    field 2 0 0 0
    field 3 1 1 1
  component 2
    field 4 "content/beauty/meshes/block.cyprim"
    field 5 "samples/05b-editor-window/project/materials/copper_clay.cygraph"
  component 3
    field 6 0.72 0.20 0.10
)";
constexpr std::string_view kShadowScene = R"(cyworld 1
type 1 runtime "Transform"
  field 1 quat "rotation" ""
  field 2 vec3 "translation" ""
  field 3 vec3 "scale" ""
type 2 runtime "MeshRenderer"
  field 4 text "mesh" ""
  field 9 bool "casts_shadow" ""
  field 10 bool "receives_shadow" ""
type 3 runtime "LightSource"
  field 5 int "kind" ""
  field 6 float "intensity" ""
  field 7 bool "enabled" ""
  field 8 bool "casts_shadow" ""
node 0 - "test" "Block"
  component 1
    field 1 0 0 0 1
    field 2 0 0 0
    field 3 1 1 1
  component 2
    field 4 "content/beauty/meshes/block.cyprim"
    field 9 true
node 1 - "test" "Plane"
  component 1
    field 1 0 0 0 1
    field 2 0 -0.04 0
    field 3 8 1 8
  component 2
    field 4 "samples/05b-editor-window/runtime/tests/assets/plane.cyprim"
    field 10 true
node 2 - "test" "Sun"
  component 1
    field 1 0 0.9238795 0.3826834 0
    field 2 0 0 0
    field 3 1 1 1
  component 3
    field 5 0
    field 6 100000
    field 7 true
    field 8 true
)";

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Gpu);
}

#if defined(__APPLE__)
constexpr const char* kBackend = "metal";
constexpr const char* kSuite = "smoke.editor_authored_frame_metal";
void register_backend() noexcept {
    (void)rhi::metal::register_metal_backend();
}
#else
constexpr const char* kBackend = "vulkan";
constexpr const char* kSuite = "smoke.editor_authored_frame_vulkan";
void register_backend() noexcept {
    (void)rhi::vulkan::register_vulkan_backend();
}
#endif

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

u64 red_sum(Span<const u32> pixels) noexcept {
    u64 sum = 0;
    for (u32 pixel : pixels) {
        sum += pixel & 0xFFU;
    }
    return sum;
}

usize darkened_pixels(Span<const u32> shadowed, Span<const u32> lit) noexcept {
    usize count = 0;
    for (usize pixel = 0; pixel < shadowed.size(); ++pixel) {
        const u32 shadow_red = shadowed[pixel] & 0xFFU;
        const u32 lit_red = lit[pixel] & 0xFFU;
        count += static_cast<usize>(shadow_red + 30U < lit_red);
    }
    return count;
}

usize differing_pixels(Span<const u32> before, Span<const u32> after) noexcept {
    usize count = 0;
    for (usize pixel = 0; pixel < before.size(); ++pixel) {
        count += static_cast<usize>(before[pixel] != after[pixel]);
    }
    return count;
}

enum class Occurrence : u8 { First, Last };

// The authored text with one line changed, as the editor would write it after one edit. The line
// must be there: a scene the replacement never touched would pass every comparison below vacuously.
std::string edited(std::string_view text, std::string_view line, std::string_view replacement,
                   Occurrence occurrence) {
    std::string out(text);
    const usize at = occurrence == Occurrence::First ? out.find(line) : out.rfind(line);
    CY_REQUIRE(at != std::string::npos);
    out.replace(at, line.size(), replacement);
    return out;
}

void read_resolved(std::string_view text, const ser::AuthoringSchema& schema, ser::World& out) {
    CY_REQUIRE(ser::read_world(text, "worlds/test.cyworld", out).has_value());
    CY_REQUIRE(ser::resolve_against(out, schema).has_value());
}

// The six scenes every stage below shares, read in the order the editor would open them.
struct BaseWorlds {
    ser::World empty{allocator()};
    ser::World sphere{allocator()};
    ser::World transformed{allocator()};
    ser::World parented{allocator()};
    ser::World lit{allocator()};
    ser::World authored_camera{allocator()};
};

void read_base_worlds(BaseWorlds& worlds) {
    CY_REQUIRE(ser::read_world(kEmpty, "worlds/test.cyworld", worlds.empty).has_value());
    CY_REQUIRE(ser::read_world(kSphere, "worlds/test.cyworld", worlds.sphere).has_value());
    CY_REQUIRE(
        ser::read_world(kTransformed, "worlds/test.cyworld", worlds.transformed).has_value());
    CY_REQUIRE(ser::read_world(kParented, "worlds/test.cyworld", worlds.parented).has_value());
    CY_REQUIRE(ser::read_world(kLit, "worlds/test.cyworld", worlds.lit).has_value());
    CY_REQUIRE(ser::read_world(kCamera, "worlds/test.cyworld", worlds.authored_camera).has_value());
}

void resolve_base_worlds(BaseWorlds& worlds, const ser::AuthoringSchema& schema) {
    CY_REQUIRE(ser::resolve_against(worlds.sphere, schema).has_value());
    CY_REQUIRE(ser::resolve_against(worlds.transformed, schema).has_value());
    CY_REQUIRE(ser::resolve_against(worlds.parented, schema).has_value());
    CY_REQUIRE(ser::resolve_against(worlds.lit, schema).has_value());
    CY_REQUIRE(ser::resolve_against(worlds.authored_camera, schema).has_value());
}

// A mesh draws, publishes its instance with its bounds, and follows its transform and its parent.
void check_mesh_and_bounds(AuthoredFrame& frame, const BaseWorlds& worlds,
                           const first_light::Camera& view) {
    CY_REQUIRE(frame.render(worlds.empty, view));
    Array<u32> blank(allocator());
    CY_REQUIRE(blank.append(frame.pixels()));
    CY_REQUIRE(frame.render(worlds.sphere, view));
    CY_CHECK(differing_pixels(blank.span(), frame.pixels()) > 100);

    Array<render::GpuInstance> instances(allocator());
    Array<render::DrawItem> draws(allocator());
    CY_REQUIRE(frame.publish(view, instances, draws));
    CY_REQUIRE_EQ(instances.size(), 1U);
    CY_REQUIRE_EQ(draws.size(), 1U);
    CY_CHECK_EQ(instances[0].stable_id(), worlds.sphere.nodes()[0].identity);
    CY_CHECK((instances[0].flags & render::kInstanceCastsShadow) != 0U);
    CY_CHECK((instances[0].flags & render::kInstanceReceivesShadow) != 0U);
    CY_CHECK(instances[0].bounds_radius > 0.6F);
    const first_light::Camera framed = frame.framing(view);
    CY_CHECK(framed.position[2] < 3.0);
    Array<u32> before_transform(allocator());
    CY_REQUIRE(before_transform.append(frame.pixels()));
    CY_REQUIRE(frame.render(worlds.transformed, view));
    CY_CHECK(differing_pixels(before_transform.span(), frame.pixels()) > 100);
    CY_REQUIRE(frame.publish(view, instances, draws));
    CY_REQUIRE_EQ(instances.size(), 1U);
    CY_CHECK(instances[0].bounds_center[0] > 1.0F);
    CY_CHECK(instances[0].bounds_radius > 1.0F);
    CY_REQUIRE(frame.render(worlds.parented, view));
    CY_REQUIRE(frame.publish(view, instances, draws));
    CY_REQUIRE_EQ(instances.size(), 1U);
    CY_CHECK_EQ(instances[0].stable_id(), worlds.parented.nodes()[1].identity);
    CY_CHECK(instances[0].bounds_center[0] > 2.0F);
    Vec3 parent_pivot;
    CY_CHECK(frame.pivot_for(worlds.parented.nodes()[0].identity, parent_pivot));
    CY_CHECK(parent_pivot.x > 1.9F);
}

// An authored camera is found, framed from its parent, and drawn as a marker.
void check_scene_camera(AuthoredFrame& frame, const BaseWorlds& worlds,
                        const first_light::Camera& view) {
    first_light::Camera game_camera;
    CY_CHECK(frame.scene_camera(worlds.authored_camera, 0, game_camera));
    CY_CHECK(game_camera.position[0] > 2.9);
    CY_CHECK(game_camera.fov_y_radians > 1.0F);
    CY_CHECK(!frame.scene_camera(worlds.empty, 0, game_camera));
    CY_REQUIRE(frame.render(worlds.authored_camera, view));
    CY_REQUIRE_EQ(frame.camera_markers().size(), 1U);
    CY_CHECK_EQ(frame.camera_markers()[0].identity, worlds.authored_camera.nodes()[1].identity);
    CY_CHECK(frame.camera_markers()[0].forward.z < -0.5F);
}

// The editor's preview lighting differs from the scene's own, and an authored point light adds red.
void check_point_light(AuthoredFrame& frame, const BaseWorlds& worlds,
                       const first_light::Camera& view) {
    CY_REQUIRE(frame.render(worlds.sphere, view, true));
    Array<u32> preview(allocator());
    CY_REQUIRE(preview.append(frame.pixels()));
    CY_REQUIRE(frame.render(worlds.sphere, view, false));
    Array<u32> unlit(allocator());
    CY_REQUIRE(unlit.append(frame.pixels()));
    CY_CHECK(differing_pixels(preview.span(), unlit.span()) > 50);
    CY_REQUIRE(frame.render(worlds.lit, view, false));
    CY_CHECK(differing_pixels(unlit.span(), frame.pixels()) > 50);
    CY_CHECK(red_sum(frame.pixels()) > red_sum(unlit.span()) + 1000U);
}

// A directional light lights the scene, switches off with `enabled`, and follows its rotation.
void check_directional_light(AuthoredFrame& frame, const ser::AuthoringSchema& schema,
                             const first_light::Camera& view) {
    const std::string pointed = edited(kLit, "field 5 1\n", "field 5 0\n", Occurrence::First);
    const std::string directional_text =
        edited(pointed, "field 6 5000\n", "field 6 100000\n", Occurrence::First);
    const std::string disabled_text =
        edited(directional_text, "field 8 true\n", "field 8 false\n", Occurrence::First);
    const std::string rotated_text =
        edited(directional_text, "field 1 0 0 0 1\n", "field 1 0 1 0 0\n", Occurrence::Last);
    ser::World directional(allocator());
    ser::World disabled(allocator());
    ser::World rotated(allocator());
    read_resolved(directional_text, schema, directional);
    read_resolved(disabled_text, schema, disabled);
    read_resolved(rotated_text, schema, rotated);
    CY_REQUIRE(frame.render(directional, view, true));
    Array<u32> lit_directional(allocator());
    CY_REQUIRE(lit_directional.append(frame.pixels()));
    CY_REQUIRE(frame.render(disabled, view, true));
    CY_REQUIRE_EQ(frame.light_markers().size(), 1U);
    CY_CHECK(red_sum(lit_directional.span()) > red_sum(frame.pixels()) + 1000U);
    CY_CHECK(differing_pixels(lit_directional.span(), frame.pixels()) > 50);
    CY_REQUIRE(frame.render(rotated, view, true));
    CY_CHECK(differing_pixels(lit_directional.span(), frame.pixels()) > 50);
}

// The sun casts a shadow only while the light, the caster and the receiver all say so, and the
// shadow moves with the sun.
void check_shadows(AuthoredFrame& frame, const ser::AuthoringSchema& schema,
                   const first_light::Camera& view) {
    ser::World shadow_on(allocator());
    ser::World shadow_off(allocator());
    read_resolved(kShadowScene, schema, shadow_on);
    read_resolved(edited(kShadowScene, "field 8 true\n", "field 8 false\n", Occurrence::Last),
                  schema, shadow_off);
    CY_REQUIRE(frame.render(shadow_on, view, true));
    Array<u32> shadowed(allocator());
    CY_REQUIRE(shadowed.append(frame.pixels()));
    CY_REQUIRE(frame.render(shadow_off, view, true));
    Array<u32> shadow_disabled(allocator());
    CY_REQUIRE(shadow_disabled.append(frame.pixels()));
    CY_CHECK(darkened_pixels(shadowed.span(), shadow_disabled.span()) > 100);

    ser::World caster_off(allocator());
    read_resolved(edited(kShadowScene, "field 9 true\n", "field 9 false\n", Occurrence::First),
                  schema, caster_off);
    CY_REQUIRE(frame.render(caster_off, view, true));
    CY_CHECK(darkened_pixels(frame.pixels(), shadow_disabled.span()) < 20);

    ser::World receiver_off(allocator());
    read_resolved(edited(kShadowScene, "field 10 true\n", "field 10 false\n", Occurrence::First),
                  schema, receiver_off);
    CY_REQUIRE(frame.render(receiver_off, view, true));
    CY_CHECK(darkened_pixels(frame.pixels(), shadow_disabled.span()) < 20);

    ser::World shadow_rotated(allocator());
    read_resolved(edited(kShadowScene, "field 1 0 0.9238795 0.3826834 0\n",
                         "field 1 -0.2705981 0.6532815 0.2705981 0.6532815\n", Occurrence::Last),
                  schema, shadow_rotated);
    CY_REQUIRE(frame.render(shadow_rotated, view, true));
    CY_CHECK(differing_pixels(shadowed.span(), frame.pixels()) > 100);
}

// A graph material draws its authored colour, a scene override replaces it, and an unsaved preview
// of the graph replaces the graph's default until the saved graph is previewed again.
void check_graph_material(AuthoredFrame& frame, const first_light::Camera& view) {
    const std::string changed_albedo = edited(kGraphMaterial, "field 6 0.72 0.20 0.10",
                                              "field 6 0.05 0.20 0.10", Occurrence::First);
    ser::World graph_default(allocator());
    ser::World graph_override(allocator());
    CY_REQUIRE(ser::read_world(kGraphMaterial, "worlds/graph.cyworld", graph_default).has_value());
    CY_REQUIRE(ser::read_world(changed_albedo, "worlds/graph.cyworld", graph_override).has_value());
    CY_REQUIRE(frame.render(graph_default, view));
    const u64 default_red = red_sum(frame.pixels());
    CY_REQUIRE(frame.render(graph_override, view));
    CY_CHECK(default_red > red_sum(frame.pixels()) + 1000U);

    const std::string reference = "samples/05b-editor-window/project/materials/copper_clay.cygraph";
    Array<u8> graph_bytes(allocator());
    CY_REQUIRE(assets::fs::read_whole((std::string(CY_TEST_PROJECT) + "/" + reference).c_str(),
                                      graph_bytes));
    const std::string graph_source(reinterpret_cast<const char*>(graph_bytes.data()),
                                   graph_bytes.size());
    const std::string preview_source =
        edited(graph_source, "0.720000029", "0.100000001", Occurrence::First);
    CY_REQUIRE(frame.preview(reference, preview_source));
    CY_REQUIRE(frame.render(graph_default, view));
    const u64 preview_red = red_sum(frame.pixels());
    CY_CHECK(default_red > preview_red + 1000U);
    CY_REQUIRE(frame.render(graph_override, view));
    CY_CHECK(preview_red > red_sum(frame.pixels()) + 1000U);
    CY_REQUIRE(frame.preview(reference, graph_source));
    CY_REQUIRE(frame.render(graph_default, view));
    const u64 restored_red = red_sum(frame.pixels());
    CY_CHECK((restored_red > default_red ? restored_red - default_red
                                         : default_red - restored_red) < 1000U);
}

}  // namespace

// One device and one frame for every stage: the stages run in this order against the same frame,
// so each one also shows the frame carries nothing over from the scene before it.
CY_TEST_CASE("authored native frame renders a mesh and publishes its transformed bounds") {
    register_backend();
    rhi::DeviceDescription description;
    description.application_name = kSuite;
    description.enable_validation = true;
    rhi::BackendSelection selection;
    auto device = rhi::create_device(allocator(), kBackend, description, selection);
    CY_REQUIRE(device.has_value());

    {
        AuthoredFrame frame(allocator(), **device);
        const auto initialized = frame.initialize(192, 128, CY_TEST_PROJECT);
        if (!initialized) {
            std::fprintf(stderr, "AuthoredFrame initialize: %s\n", initialized.error().message);
        }
        CY_REQUIRE(initialized);
        BaseWorlds worlds;
        read_base_worlds(worlds);
        reflect::TypeRegistry registry;
        CY_REQUIRE(reflect::register_scene_types(registry));
        ser::AuthoringSchema schema(allocator());
        CY_REQUIRE(ser::build_authoring_schema(registry, schema));
        resolve_base_worlds(worlds, schema);
        const first_light::Camera view = camera();

        check_mesh_and_bounds(frame, worlds, view);
        check_scene_camera(frame, worlds, view);
        check_point_light(frame, worlds, view);
        check_directional_light(frame, schema, view);
        check_shadows(frame, schema, view);
        check_graph_material(frame, view);
    }
    rhi::destroy_device(allocator(), *device);
}
