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
#if defined(CY_EDITOR_WINDOW_HAS_VFX)
#    include <cy/editor/material_service.h>
#endif
#include <cy/scene/serialization/worldfile.h>
#include <cy/test/test.h>
#include <cy_reflect_generated_scene.h>

#include "authored_frame.h"
#if defined(CY_EDITOR_WINDOW_HAS_VFX)
#    include "golden.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace cy;
using namespace cy::sample::editor_window;
namespace ser = cy::scene::serialization;
namespace first_light = cy::sample::first_light;

namespace {

constexpr std::string_view kEmpty = "cyworld 1\n";
constexpr std::string_view kVertexGraph = R"(cygraph 1
graph "offset" version 1
capability
deterministic true
node 1 "material.vertex_output" v1 {
}
)";
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

}  // namespace

CY_TEST_CASE("authored scene material path names its unsupported vertex output") {
    std::ifstream source(CY_TEST_PROJECT
                         "/samples/05b-editor-window/project/materials/copper_clay.cygraph");
    CY_REQUIRE(source.good());
    std::ostringstream contents;
    contents << source.rdbuf();
    const std::string surface = contents.str();
    auto accepted = graph_diffuse_colour(surface, allocator());
    CY_REQUIRE(accepted.has_value());

    auto rejected = graph_diffuse_colour(kVertexGraph, allocator());
    CY_REQUIRE_FALSE(rejected.has_value());
    CY_CHECK_EQ(rejected.error().code, ErrorCode::Unsupported);
    CY_CHECK(std::string_view(rejected.error().message).find("vertex-offset material pass") !=
             std::string_view::npos);
}

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
        CY_CHECK((instances[0].flags & render::kInstanceCastsShadow) != 0U);
        CY_CHECK((instances[0].flags & render::kInstanceReceivesShadow) != 0U);
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
        CY_REQUIRE(frame.render(authored_camera, view));
        CY_REQUIRE_EQ(frame.camera_markers().size(), 1U);
        CY_CHECK_EQ(frame.camera_markers()[0].identity, authored_camera.nodes()[1].identity);
        CY_CHECK(frame.camera_markers()[0].forward.z < -0.5F);

        CY_REQUIRE(frame.render(sphere, view, true));
        Array<u32> preview(allocator());
        CY_REQUIRE(preview.append(frame.pixels()));
        CY_REQUIRE(frame.render(sphere, view, false));
        Array<u32> unlit(allocator());
        CY_REQUIRE(unlit.append(frame.pixels()));
        usize preview_pixels = 0;
        for (usize pixel = 0; pixel < preview.size(); ++pixel) {
            preview_pixels += static_cast<usize>(preview[pixel] != unlit[pixel]);
        }
        CY_CHECK(preview_pixels > 50);
        CY_REQUIRE(frame.render(lit, view, false));
        usize lighting_changed = 0;
        for (usize pixel = 0; pixel < unlit.size(); ++pixel) {
            lighting_changed += static_cast<usize>(unlit[pixel] != frame.pixels()[pixel]);
        }
        CY_CHECK(lighting_changed > 50);
        CY_CHECK(red_sum(frame.pixels()) > red_sum(unlit.span()) + 1000U);

        std::string directional_text(kLit);
        const usize kind_at = directional_text.find("field 5 1\n");
        const usize intensity_at = directional_text.find("field 6 5000\n");
        CY_REQUIRE(kind_at != std::string::npos);
        CY_REQUIRE(intensity_at != std::string::npos);
        directional_text.replace(intensity_at, sizeof("field 6 5000\n") - 1, "field 6 100000\n");
        directional_text.replace(kind_at, sizeof("field 5 1\n") - 1, "field 5 0\n");
        std::string disabled_text = directional_text;
        const usize enabled_at = disabled_text.find("field 8 true\n");
        CY_REQUIRE(enabled_at != std::string::npos);
        disabled_text.replace(enabled_at, sizeof("field 8 true\n") - 1, "field 8 false\n");
        std::string rotated_text = directional_text;
        const usize light_rotation_at = rotated_text.rfind("field 1 0 0 0 1\n");
        CY_REQUIRE(light_rotation_at != std::string::npos);
        rotated_text.replace(light_rotation_at, sizeof("field 1 0 0 0 1\n") - 1,
                             "field 1 0 1 0 0\n");
        ser::World directional(allocator());
        ser::World disabled(allocator());
        ser::World rotated(allocator());
        CY_REQUIRE(
            ser::read_world(directional_text, "worlds/test.cyworld", directional).has_value());
        CY_REQUIRE(ser::read_world(disabled_text, "worlds/test.cyworld", disabled).has_value());
        CY_REQUIRE(ser::read_world(rotated_text, "worlds/test.cyworld", rotated).has_value());
        CY_REQUIRE(ser::resolve_against(directional, schema).has_value());
        CY_REQUIRE(ser::resolve_against(disabled, schema).has_value());
        CY_REQUIRE(ser::resolve_against(rotated, schema).has_value());
        CY_REQUIRE(frame.render(directional, view, true));
        Array<u32> lit_directional(allocator());
        CY_REQUIRE(lit_directional.append(frame.pixels()));
        CY_REQUIRE(frame.render(disabled, view, true));
        CY_REQUIRE_EQ(frame.light_markers().size(), 1U);
        CY_CHECK(red_sum(lit_directional.span()) > red_sum(frame.pixels()) + 1000U);
        usize switched_pixels = 0;
        for (usize pixel = 0; pixel < lit_directional.size(); ++pixel) {
            switched_pixels += static_cast<usize>(lit_directional[pixel] != frame.pixels()[pixel]);
        }
        CY_CHECK(switched_pixels > 50);
        CY_REQUIRE(frame.render(rotated, view, true));
        usize rotated_pixels = 0;
        for (usize pixel = 0; pixel < lit_directional.size(); ++pixel) {
            rotated_pixels += static_cast<usize>(lit_directional[pixel] != frame.pixels()[pixel]);
        }
        CY_CHECK(rotated_pixels > 50);

        std::string shadow_off_text(kShadowScene);
        const usize shadow_flag = shadow_off_text.rfind("field 8 true\n");
        CY_REQUIRE(shadow_flag != std::string::npos);
        shadow_off_text.replace(shadow_flag, sizeof("field 8 true\n") - 1, "field 8 false\n");
        ser::World shadow_on(allocator());
        ser::World shadow_off(allocator());
        CY_REQUIRE(ser::read_world(kShadowScene, "worlds/test.cyworld", shadow_on).has_value());
        CY_REQUIRE(ser::read_world(shadow_off_text, "worlds/test.cyworld", shadow_off).has_value());
        CY_REQUIRE(ser::resolve_against(shadow_on, schema).has_value());
        CY_REQUIRE(ser::resolve_against(shadow_off, schema).has_value());
        CY_REQUIRE(frame.render(shadow_on, view, true));
        Array<u32> shadowed(allocator());
        CY_REQUIRE(shadowed.append(frame.pixels()));
        CY_REQUIRE(frame.render(shadow_off, view, true));
        Array<u32> shadow_disabled(allocator());
        CY_REQUIRE(shadow_disabled.append(frame.pixels()));
        CY_CHECK(darkened_pixels(shadowed.span(), shadow_disabled.span()) > 100);

        std::string caster_off_text(kShadowScene);
        const usize caster_flag = caster_off_text.find("field 9 true\n");
        CY_REQUIRE(caster_flag != std::string::npos);
        caster_off_text.replace(caster_flag, sizeof("field 9 true\n") - 1, "field 9 false\n");
        ser::World caster_off(allocator());
        CY_REQUIRE(ser::read_world(caster_off_text, "worlds/test.cyworld", caster_off).has_value());
        CY_REQUIRE(ser::resolve_against(caster_off, schema).has_value());
        CY_REQUIRE(frame.render(caster_off, view, true));
        CY_CHECK(darkened_pixels(frame.pixels(), shadow_disabled.span()) < 20);

        std::string receiver_off_text(kShadowScene);
        const usize receiver_flag = receiver_off_text.find("field 10 true\n");
        CY_REQUIRE(receiver_flag != std::string::npos);
        receiver_off_text.replace(receiver_flag, sizeof("field 10 true\n") - 1, "field 10 false\n");
        ser::World receiver_off(allocator());
        CY_REQUIRE(
            ser::read_world(receiver_off_text, "worlds/test.cyworld", receiver_off).has_value());
        CY_REQUIRE(ser::resolve_against(receiver_off, schema).has_value());
        CY_REQUIRE(frame.render(receiver_off, view, true));
        CY_CHECK(darkened_pixels(frame.pixels(), shadow_disabled.span()) < 20);
        std::string shadow_rotated_text(kShadowScene);
        const usize shadow_rotation =
            shadow_rotated_text.rfind("field 1 0 0.9238795 0.3826834 0\n");
        CY_REQUIRE(shadow_rotation != std::string::npos);
        shadow_rotated_text.replace(shadow_rotation,
                                    sizeof("field 1 0 0.9238795 0.3826834 0\n") - 1,
                                    "field 1 -0.2705981 0.6532815 0.2705981 0.6532815\n");
        ser::World shadow_rotated(allocator());
        CY_REQUIRE(ser::read_world(shadow_rotated_text, "worlds/test.cyworld", shadow_rotated)
                       .has_value());
        CY_REQUIRE(ser::resolve_against(shadow_rotated, schema).has_value());
        CY_REQUIRE(frame.render(shadow_rotated, view, true));
        usize moved_shadow_pixels = 0;
        for (usize pixel = 0; pixel < shadowed.size(); ++pixel) {
            moved_shadow_pixels += static_cast<usize>(shadowed[pixel] != frame.pixels()[pixel]);
        }
        CY_CHECK(moved_shadow_pixels > 100);

        std::string changed_albedo(kGraphMaterial);
        const usize albedo_at = changed_albedo.find("field 6 0.72 0.20 0.10");
        CY_REQUIRE(albedo_at != std::string::npos);
        changed_albedo.replace(albedo_at, sizeof("field 6 0.72 0.20 0.10") - 1,
                               "field 6 0.05 0.20 0.10");
        ser::World graph_default(allocator());
        ser::World graph_override(allocator());
        CY_REQUIRE(
            ser::read_world(kGraphMaterial, "worlds/graph.cyworld", graph_default).has_value());
        CY_REQUIRE(
            ser::read_world(changed_albedo, "worlds/graph.cyworld", graph_override).has_value());
        CY_REQUIRE(frame.render(graph_default, view));
        const u64 default_red = red_sum(frame.pixels());
        CY_REQUIRE(frame.render(graph_override, view));
        CY_CHECK(default_red > red_sum(frame.pixels()) + 1000U);

        const std::string reference =
            "samples/05b-editor-window/project/materials/copper_clay.cygraph";
        Array<u8> graph_bytes(allocator());
        CY_REQUIRE(assets::fs::read_whole((std::string(CY_TEST_PROJECT) + "/" + reference).c_str(),
                                          graph_bytes));
        std::string graph_source(reinterpret_cast<const char*>(graph_bytes.data()),
                                 graph_bytes.size());
        std::string preview_source = graph_source;
        const usize colour_at = preview_source.find("0.720000029");
        CY_REQUIRE(colour_at != std::string::npos);
        preview_source.replace(colour_at, sizeof("0.720000029") - 1, "0.100000001");
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
    rhi::destroy_device(allocator(), *device);
}

#if defined(CY_EDITOR_WINDOW_HAS_VFX)
CY_TEST_CASE("authored Metal viewport composites the engine VFX preview") {
    (void)rhi::metal::register_metal_backend();
    rhi::DeviceDescription description;
    description.application_name = "smoke.editor_vfx_preview_metal";
    rhi::BackendSelection selection;
    auto device =
        rhi::create_device(allocator(), rhi::metal::kMetalBackendName, description, selection);
    CY_REQUIRE(device.has_value());
    {
        AuthoredFrame frame(allocator(), **device);
        CY_REQUIRE(frame.initialize(640, 360, CY_TEST_PROJECT, false));
        ser::World empty(allocator());
        CY_REQUIRE(ser::read_world(kEmpty, "worlds/test.cyworld", empty).has_value());
        const first_light::Camera view = camera();
        CY_REQUIRE(frame.render(empty, view, false));
        Array<u32> baseline(allocator());
        CY_REQUIRE(baseline.append(frame.pixels()));

        const std::string path =
            std::string(CY_TEST_PROJECT) +
            "/samples/05b-editor-window/project/effects/issue15_two_emitters.cyvfxdoc";
        std::ifstream file(path);
        CY_REQUIRE(file.good());
        const std::string source(std::istreambuf_iterator<char>{file}, {});
        editor::MaterialService service(allocator());
        CyServiceSession session = nullptr;
        CY_REQUIRE_EQ(service.open(&session), CY_RESULT_OK);
        u64 request_id = 1;
        const auto call = [&](const char* operation, const std::vector<u8>& payload) {
            const CyServiceRequest request{sizeof(CyServiceRequest),
                                           1,
                                           request_id++,
                                           operation,
                                           payload.data(),
                                           payload.size()};
            CY_REQUIRE_EQ(service.submit(session, request), CY_RESULT_OK);
            CyServiceEvent event{};
            bool present = false;
            CY_REQUIRE_EQ(service.poll(session, event, present), CY_RESULT_OK);
            CY_REQUIRE(present);
            CY_REQUIRE_EQ(event.kind, static_cast<u32>(CY_SERVICE_EVENT_COMPLETED));
        };
        call("vfx.preview.load", {source.begin(), source.end()});
        call("vfx.preview.control", {0});
        f32 seconds = 1.0F / 30.0F;
        u32 bits = 0;
        std::memcpy(&bits, &seconds, sizeof(bits));
        std::vector<u8> interval{static_cast<u8>(bits), static_cast<u8>(bits >> 8U),
                                 static_cast<u8>(bits >> 16U), static_cast<u8>(bits >> 24U)};
        for (u32 frame_index = 0; frame_index < 15; ++frame_index) {
            call("vfx.preview.step", interval);
        }
        const vfx::SimulationWorld* preview = service.vfx_preview_world(session);
        CY_REQUIRE(preview != nullptr);
        CY_REQUIRE(frame.render(empty, view, false, preview));
        CY_CHECK_GT(frame.vfx_particle_report().particles, 0U);
        CY_CHECK_EQ(frame.vfx_particle_report().draws, 1U);
        const auto records = frame.vfx_records();
        CY_REQUIRE(!records.empty());
        CY_CHECK_EQ(records[0].size, 0.22F);
        CY_CHECK_GT(records[0].color[3], 0.0F);
        usize changed = 0;
        for (usize pixel = 0; pixel < baseline.size(); ++pixel) {
            changed += static_cast<usize>(baseline[pixel] != frame.pixels()[pixel]);
        }
        CY_CHECK_GT(changed, 20U);
        render_test::Image captured(allocator());
        CY_REQUIRE(render_test::adopt(captured, frame.pixels(), 640, 360).has_value());
        const std::string reference_path =
            std::string(CY_TEST_PROJECT) +
            "/samples/05b-editor-window/runtime/tests/references/issue15_two_emitters_metal.png";
        const char* update = std::getenv("CY_RENDER_UPDATE_GOLDEN");
        if (update != nullptr && update[0] != '\0' && update[0] != '0') {
            CY_REQUIRE(render_test::write_png(reference_path.c_str(), captured).has_value());
            std::fprintf(stderr, "Updated %s; inspect and commit the image.\n",
                         reference_path.c_str());
            CY_CHECK_FALSE(update != nullptr);  // A reference update cannot pass the test.
        } else {
            render_test::Image reference(allocator());
            const Status read = render_test::read_png(reference_path.c_str(), reference);
            if (!read) {
                std::fprintf(stderr, "VFX reference %s: %s\n", reference_path.c_str(),
                             read.error().message);
            }
            CY_REQUIRE(read.has_value());
            const render_test::Comparison comparison = render_test::compare(reference, captured);
            CY_REQUIRE(comparison.comparable);
            if (comparison.differing != 0) {
                (void)render_test::write_difference("issue15-two-emitters-metal-difference.png",
                                                    reference, captured);
                std::fprintf(stderr,
                             "VFX image: %u differing texels, %u away from edges; worst channel "
                             "delta %u at (%u, %u).\n",
                             comparison.differing, comparison.differing_off_edge,
                             comparison.max_channel_delta, comparison.worst_x, comparison.worst_y);
            }
            CY_CHECK_EQ(comparison.differing_off_edge, 0U);
            CY_CHECK_LE(comparison.differing, comparison.edge_texels);
        }
        CY_REQUIRE(frame.render(empty, view, false));
        CY_CHECK_EQ(frame.vfx_particle_report().particles, 0U);
        CY_CHECK_EQ(frame.vfx_particle_report().draws, 0U);
        usize residual = 0;
        for (usize pixel = 0; pixel < baseline.size(); ++pixel) {
            residual += static_cast<usize>(baseline[pixel] != frame.pixels()[pixel]);
        }
        CY_CHECK_EQ(residual, 0U);
        service.close(session);
    }
    rhi::destroy_device(allocator(), *device);
}
#endif
