// SPDX-License-Identifier: MIT
// The editor's authored frame on the native device this host publishes from: Metal on Apple,
// Vulkan on Linux. One file so both backends answer to the same pixel assertions.
#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
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
#include "material_runtime.h"
#if defined(CY_EDITOR_WINDOW_HAS_VFX)
#    include "golden.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
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
constexpr std::string_view kInterpolantGraph = R"(cygraph 1
graph "interpolant" version 1
capability
deterministic true
node 1 "material.vertex_interpolant" v1 {
}
)";
constexpr std::string_view kSurfaceVertexGraph = R"(cygraph 1
graph "scene_sway" version 1
capability
deterministic true
node 1 "material.constant" v1 {
    prop "type" : "name" = "float3"
    prop "value" : "vec4" = (0.7, 0.5, 0.2, 0, 0)
}
node 2 "material.constant" v1 {
    prop "type" : "name" = "float"
    prop "value" : "vec4" = (1, 0, 0, 0, 0)
}
node 3 "material.diffuse" v1 {
}
node 4 "material.output" v1 {
}
node 5 "material.constant" v1 {
    prop "type" : "name" = "float3"
    prop "value" : "vec4" = (0, 0.25, 0, 0, 0)
}
node 6 "material.vertex_output" v1 {
}
link 1 "out" -> 3 "colour"
link 2 "out" -> 3 "weight"
link 2 "out" -> 4 "opacity"
link 3 "out" -> 4 "surface"
link 5 "out" -> 6 "offset"
)";
constexpr std::string_view kSceneInterpolantGraph = R"(cygraph 1
graph "scene_interpolant" version 1
capability
deterministic true
node 1 "material.object_position" v1 {
}
node 2 "material.vertex_interpolant" v1 {
    prop "symbol" : "name" = "tint"
}
node 3 "material.attribute" v1 {
    prop "symbol" : "name" = "tint"
    prop "type" : "name" = "float3"
}
node 4 "material.diffuse" v1 {
}
node 5 "material.output" v1 {
}
link 1 "out" -> 2 "value"
link 3 "out" -> 4 "colour"
link 4 "out" -> 5 "surface"
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
constexpr rhi::BackendKind kNativeBackend = rhi::BackendKind::Metal;
constexpr const char* kSuite = "smoke.editor_authored_frame_metal";
void register_backend() noexcept {
    (void)rhi::metal::register_metal_backend();
}
#else
constexpr const char* kBackend = "vulkan";
constexpr rhi::BackendKind kNativeBackend = rhi::BackendKind::Vulkan;
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

#if defined(CY_SHADER_SLANG) && CY_SHADER_SLANG
std::string assigned_vertex_shadow_scene() {
    const std::string typed = edited(kShadowScene, "  field 4 text \"mesh\" \"\"\n",
                                     "  field 4 text \"mesh\" \"\"\n"
                                     "  field 11 text \"material\" \"\"\n",
                                     Occurrence::First);
    return edited(
        typed, "    field 4 \"content/beauty/meshes/block.cyprim\"\n    field 9 true\n",
        "    field 4 \"content/beauty/meshes/block.cyprim\"\n"
        "    field 11 \"samples/05b-editor-window/project/materials/copper_clay.cygraph\"\n"
        "    field 9 true\n",
        Occurrence::First);
}

std::string time_vertex_graph() {
    const std::string with_nodes = edited(kSurfaceVertexGraph, "link 1 \"out\" -> 3 \"colour\"\n",
                                          "node 7 \"material.time\" v1 {\n}\n"
                                          "node 8 \"material.sin\" v1 {\n}\n"
                                          "node 9 \"material.constant\" v1 {\n"
                                          "    prop \"type\" : \"name\" = \"float3\"\n"
                                          "    prop \"value\" : \"vec4\" = (0, 4, 0, 0, 0)\n}\n"
                                          "node 10 \"material.multiply\" v1 {\n}\n"
                                          "link 1 \"out\" -> 3 \"colour\"\n",
                                          Occurrence::First);
    return edited(with_nodes, "link 5 \"out\" -> 6 \"offset\"\n",
                  "link 7 \"out\" -> 8 \"value\"\n"
                  "link 9 \"out\" -> 10 \"a\"\n"
                  "link 8 \"out\" -> 10 \"b\"\n"
                  "link 10 \"out\" -> 6 \"offset\"\n",
                  Occurrence::First);
}

// The graph's vertical offset must make the same visible mesh and shadow as moving the source
// mesh on the CPU. Previewing a zero offset keeps the surface graph and material settings equal.
void check_graph_displacement_matches_cpu(AuthoredFrame& frame, const ser::AuthoringSchema& schema,
                                          const first_light::Camera& view) {
    const std::string reference = "samples/05b-editor-window/project/materials/copper_clay.cygraph";
    const std::string assigned = assigned_vertex_shadow_scene();
    const std::string raised =
        edited(assigned, "    field 2 0 0 0\n", "    field 2 0 0.25 0\n", Occurrence::First);
    ser::World source(allocator());
    ser::World cpu_displaced(allocator());
    read_resolved(assigned, schema, source);
    read_resolved(raised, schema, cpu_displaced);

    CY_REQUIRE(frame.preview(reference, kSurfaceVertexGraph));
    CY_REQUIRE(frame.render(source, view));
    Array<u32> graph_pixels(allocator());
    CY_REQUIRE(graph_pixels.append(frame.pixels()));

    const std::string zero_offset =
        edited(kSurfaceVertexGraph, "(0, 0.25, 0, 0, 0)", "(0, 0, 0, 0, 0)", Occurrence::First);
    CY_REQUIRE(frame.preview(reference, zero_offset));
    CY_REQUIRE(frame.render(source, view));
    CY_CHECK_GT(differing_pixels(graph_pixels.span(), frame.pixels()), 100U);
    CY_REQUIRE(frame.render(cpu_displaced, view));
    CY_CHECK_LE(differing_pixels(graph_pixels.span(), frame.pixels()), 32U);
}

// The second frame's sine displacement equals the CPU's scene translation. TAA consumes the depth
// pass's motion target, so matching the temporal image also checks the shader's previous-time
// evaluation against the CPU reference.
void check_graph_motion_matches_cpu(rhi::Device& device, const ser::AuthoringSchema& schema,
                                    const first_light::Camera& view) {
    constexpr std::string_view reference =
        "samples/05b-editor-window/project/materials/copper_clay.cygraph";
    const std::string assigned = assigned_vertex_shadow_scene();
    const std::string raised = assigned;
    constexpr f32 second_time = 0.05F;
    constexpr f32 cpu_offset = 0.19991669F;  // 4 * sin(0.05)
    const std::string graph_moved = assigned;
    const std::string cpu_moved =
        edited(raised, "    field 2 0 0 0\n", "    field 2 0 0.19991669 0\n", Occurrence::First);
    ser::World graph_before(allocator());
    ser::World cpu_before(allocator());
    ser::World graph_after(allocator());
    ser::World cpu_after(allocator());
    read_resolved(assigned, schema, graph_before);
    read_resolved(raised, schema, cpu_before);
    read_resolved(graph_moved, schema, graph_after);
    read_resolved(cpu_moved, schema, cpu_after);

    AuthoredFrame graph_frame(allocator(), device);
    AuthoredFrame cpu_frame(allocator(), device);
    CY_REQUIRE(graph_frame.initialize(192, 128, CY_TEST_PROJECT, true, true));
    CY_REQUIRE(cpu_frame.initialize(192, 128, CY_TEST_PROJECT, true, true));
    const std::string sine_graph = time_vertex_graph();
    CY_REQUIRE(graph_frame.preview(reference, sine_graph));
    const std::string zero_offset =
        edited(kSurfaceVertexGraph, "(0, 0.25, 0, 0, 0)", "(0, 0, 0, 0, 0)", Occurrence::First);
    CY_REQUIRE(cpu_frame.preview(reference, zero_offset));

    CY_REQUIRE(graph_frame.render(graph_before, view, true, nullptr, 0.0F));
    CY_REQUIRE(cpu_frame.render(cpu_before, view, true, nullptr, 0.0F));
    CY_CHECK_LE(differing_pixels(graph_frame.pixels(), cpu_frame.pixels()), 32U);
    Array<u32> graph_first(allocator());
    CY_REQUIRE(graph_first.append(graph_frame.pixels()));

    CY_CHECK_EQ(cpu_offset, doctest::Approx(4.0F * std::sin(second_time)));
    CY_REQUIRE(graph_frame.render(graph_after, view, true, nullptr, second_time));
    CY_REQUIRE(cpu_frame.render(cpu_after, view, true, nullptr, second_time));
    CY_CHECK_GT(differing_pixels(graph_first.span(), graph_frame.pixels()), 100U);
    CY_CHECK_LE(differing_pixels(graph_frame.pixels(), cpu_frame.pixels()), 32U);
    CY_REQUIRE_EQ(graph_frame.motion_texels().size(), graph_frame.pixels().size());
    CY_REQUIRE_EQ(cpu_frame.motion_texels().size(), cpu_frame.pixels().size());
    usize moving = 0;
    for (u32 texel : graph_frame.motion_texels()) {
        moving += static_cast<usize>(texel != 0);
    }
    CY_CHECK_GT(moving, 20U);
    CY_CHECK_LE(differing_pixels(graph_frame.motion_texels(), cpu_frame.motion_texels()), 32U);
}
#endif

}  // namespace

CY_TEST_CASE("authored scene material path names unsupported vertex-stage outputs") {
    std::ifstream source(CY_TEST_PROJECT
                         "/samples/05b-editor-window/project/materials/copper_clay.cygraph");
    CY_REQUIRE(source.good());
    std::ostringstream contents;
    contents << source.rdbuf();
    const std::string surface = contents.str();
    auto accepted = graph_diffuse_colour(surface, allocator());
    CY_REQUIRE(accepted.has_value());

    for (std::string_view graph : {kVertexGraph, kInterpolantGraph}) {
        auto rejected = graph_diffuse_colour(graph, allocator());
        CY_REQUIRE_FALSE(rejected.has_value());
        CY_CHECK_EQ(rejected.error().code, ErrorCode::Unsupported);
        CY_CHECK(std::string_view(rejected.error().message).find("vertex-stage material pass") !=
                 std::string_view::npos);
    }
}

CY_TEST_CASE("authored scene compiles a surface beside its vertex graph") {
    auto refused = graph_diffuse_colour(kSurfaceVertexGraph, allocator());
    CY_REQUIRE_FALSE(refused.has_value());
    auto colour = graph_diffuse_colour(kSurfaceVertexGraph, allocator(), true);
    CY_REQUIRE(colour.has_value());
    CY_CHECK(colour->vertex);
    CY_CHECK_EQ(colour->value.x, doctest::Approx(1.0F));
    CY_CHECK_EQ(colour->value.y, doctest::Approx(1.0F));
    CY_CHECK_EQ(colour->value.z, doctest::Approx(1.0F));
    auto compiled = compile_scene_graph_material(kSurfaceVertexGraph, allocator());
    CY_REQUIRE(compiled.has_value());
    const auto* program = compiled->find(rendering::material::ProgramKind::Primary,
                                         rendering::material::QualityTier::High);
    CY_REQUIRE(program != nullptr);
    auto stages = compile_scene_material_vertices(*program, allocator());
    CY_REQUIRE(stages.has_value());
    CY_CHECK_GT(stages->visible.bytes().size(), 0U);
    CY_CHECK_GT(stages->depth.bytes().size(), 0U);
    CY_CHECK_GT(stages->shadow.bytes().size(), 0U);
    auto spirv = compile_scene_material_vertices(*program, allocator(), shader::Target::SpirV);
    CY_REQUIRE(spirv.has_value());
    CY_CHECK_GT(spirv->visible.bytes().size(), 0U);
    CY_CHECK_GT(spirv->depth.bytes().size(), 0U);
    CY_CHECK_GT(spirv->shadow.bytes().size(), 0U);
    CY_CHECK_GT(spirv->fragment.bytes().size(), 0U);
#if defined(CY_SHADER_SLANG) && CY_SHADER_SLANG
    auto animated = compile_scene_graph_material(time_vertex_graph(), allocator());
    CY_REQUIRE(animated.has_value());
    const auto* animated_program = animated->find(rendering::material::ProgramKind::Primary,
                                                  rendering::material::QualityTier::High);
    CY_REQUIRE(animated_program != nullptr);
    auto animated_stages = compile_scene_material_vertices(*animated_program, allocator());
    CY_REQUIRE(animated_stages.has_value());
    CY_CHECK_GT(animated_stages->depth.bytes().size(), 0U);
    Array<char> animated_unit(allocator());
    CY_REQUIRE(assemble_scene_material_vertex_unit(*animated_program, animated_unit));
    const std::string_view source(animated_unit.data(), animated_unit.size());
    CY_CHECK(source.find("sin(") != std::string_view::npos);
    CY_CHECK(source.find("sceneMaterialTime() - sceneMaterialDelta()") != std::string_view::npos);
#endif
}

CY_TEST_CASE("authored scene graph lowers an interpolant into its forward fragment") {
    auto colour = graph_diffuse_colour(kSceneInterpolantGraph, allocator(), true);
    CY_REQUIRE(colour.has_value());
    CY_CHECK(colour->vertex);
    auto compiled = compile_scene_graph_material(kSceneInterpolantGraph, allocator());
    CY_REQUIRE(compiled.has_value());
    const auto* program = compiled->find(rendering::material::ProgramKind::Primary,
                                         rendering::material::QualityTier::High);
    CY_REQUIRE(program != nullptr);
    CY_REQUIRE_EQ(program->module.vertex_interpolants().size(), 1U);
    auto stages = compile_scene_material_vertices(*program, allocator());
    CY_REQUIRE(stages.has_value());
    CY_CHECK_GT(stages->visible.bytes().size(), 0U);
    CY_CHECK_GT(stages->fragment.bytes().size(), 0U);
}

CY_TEST_CASE("authored frame refuses nonfinite material animation time") {
    (void)rhi::null::register_null_backend();
    rhi::DeviceDescription description;
    description.application_name = "editor animation time validation";
    rhi::BackendSelection selection;
    auto device = rhi::create_device(allocator(), rhi::kNullBackendName, description, selection);
    CY_REQUIRE(device.has_value());
    {
        AuthoredFrame frame(allocator(), **device);
        CY_REQUIRE(frame.initialize(64, 64, CY_TEST_PROJECT, true, true));
        ser::World empty(allocator());
        CY_REQUIRE(ser::read_world(kEmpty, "worlds/empty.cyworld", empty).has_value());
        const Status rendered =
            frame.render(empty, camera(), true, nullptr, std::numeric_limits<f32>::infinity());
        CY_REQUIRE_FALSE(rendered.has_value());
        CY_CHECK_EQ(rendered.error().code, ErrorCode::InvalidArgument);
        CY_REQUIRE(frame.render(empty, camera(), true, nullptr, 0.0F));
        CY_CHECK_EQ(frame.motion_texels().size(), 64U * 64U);
        usize copies = 0;
        for (const auto& command : rhi::null::command_log(**device)) {
            copies +=
                static_cast<usize>(command.kind == rhi::null::CommandKind::CopyTextureToBuffer);
        }
        CY_CHECK_EQ(copies, 2U);
    }
    rhi::destroy_device(allocator(), *device);
}

#if defined(CY_SHADER_SLANG) && CY_SHADER_SLANG
CY_TEST_CASE("authored scene selects compiled vertex pipelines for its graph material") {
    (void)rhi::null::register_null_backend();
    rhi::DeviceDescription description;
    description.application_name = "editor scene vertex graph null regression";
    rhi::BackendSelection selection;
    auto device = rhi::create_device(allocator(), rhi::kNullBackendName, description, selection);
    CY_REQUIRE(device.has_value());
    {
        AuthoredFrame frame(allocator(), **device);
        CY_REQUIRE(frame.initialize(160, 90, CY_TEST_PROJECT));
        ser::World world(allocator());
        CY_REQUIRE(ser::read_world(kGraphMaterial, "worlds/graph.cyworld", world).has_value());
        reflect::TypeRegistry types;
        CY_REQUIRE(reflect::register_scene_types(types));
        ser::AuthoringSchema schema(allocator());
        CY_REQUIRE(ser::build_authoring_schema(types, schema));
        CY_REQUIRE(ser::resolve_against(world, schema).has_value());
        const first_light::Camera view = camera();
        CY_REQUIRE(frame.render(world, view));
        std::vector<u64> standard;
        for (const auto& command : rhi::null::command_log(**device)) {
            if (command.kind == rhi::null::CommandKind::BindGraphicsPipeline) {
                standard.push_back(command.handle_bits);
            }
        }
        const std::string reference =
            "samples/05b-editor-window/project/materials/copper_clay.cygraph";
        CY_REQUIRE(frame.preview(reference, kSurfaceVertexGraph));
        rhi::null::clear_command_log(**device);
        const Status rendered = frame.render(world, view);
        if (!rendered) {
            std::fprintf(stderr, "scene vertex graph: %s\n", rendered.error().message);
        }
        CY_REQUIRE(rendered);
        std::vector<u64> selected;
        for (const auto& command : rhi::null::command_log(**device)) {
            if (command.kind == rhi::null::CommandKind::BindGraphicsPipeline &&
                std::ranges::find(standard, command.handle_bits) == standard.end()) {
                selected.push_back(command.handle_bits);
            }
        }
        CY_CHECK_GE(selected.size(), 2U);

        const u64 identity = world.nodes()[0].identity;
        rendering::pipeline::InstanceTransform previous;
        CY_REQUIRE(frame.previous_material_transform(identity, previous));
        CY_CHECK_EQ(previous.row0[3], doctest::Approx(-view.position[0]));
        const std::string moved_source =
            edited(kGraphMaterial, "field 2 0 0 0", "field 2 2 0 0", Occurrence::First);
        ser::World moved(allocator());
        CY_REQUIRE(ser::read_world(moved_source, "worlds/graph.cyworld", moved).has_value());
        CY_REQUIRE(ser::resolve_against(moved, schema).has_value());
        CY_REQUIRE(frame.render(moved, view));
        CY_REQUIRE(frame.previous_material_transform(identity, previous));
        CY_CHECK_EQ(previous.row0[3], doctest::Approx(-view.position[0]));
        CY_REQUIRE(frame.render(moved, view));
        CY_REQUIRE(frame.previous_material_transform(identity, previous));
        CY_CHECK_EQ(previous.row0[3], doctest::Approx(2.0 - view.position[0]));
        first_light::Camera moved_view = view;
        moved_view.position[0] += 1.0;
        CY_REQUIRE(frame.render(moved, moved_view));
        CY_REQUIRE(frame.previous_material_transform(identity, previous));
        CY_CHECK_EQ(previous.row0[3], doctest::Approx(2.0 - view.position[0]));
        CY_REQUIRE(frame.render(moved, moved_view));
        CY_REQUIRE(frame.previous_material_transform(identity, previous));
        CY_CHECK_EQ(previous.row0[3], doctest::Approx(2.0 - moved_view.position[0]));

        const std::string two_objects =
            std::string(kGraphMaterial) + R"(node 1 - "test" "Second Block"
  component 1
    field 1 0 0 0 1
    field 2 4 0 0
    field 3 1 1 1
  component 2
    field 4 "content/beauty/meshes/block.cyprim"
    field 5 "samples/05b-editor-window/project/materials/copper_clay.cygraph"
)";
        ser::World pair(allocator());
        CY_REQUIRE(ser::read_world(two_objects, "worlds/graph.cyworld", pair).has_value());
        CY_REQUIRE(ser::resolve_against(pair, schema).has_value());
        CY_REQUIRE(frame.render(pair, view));
        const u64 second_identity = pair.nodes()[1].identity;
        CY_REQUIRE(frame.previous_material_transform(second_identity, previous));
        CY_CHECK_EQ(previous.row0[3], doctest::Approx(4.0 - view.position[0]));
        const std::string moved_pair_source =
            edited(two_objects, "field 2 0 0 0", "field 2 2 0 0", Occurrence::First);
        ser::World moved_pair(allocator());
        CY_REQUIRE(
            ser::read_world(moved_pair_source, "worlds/graph.cyworld", moved_pair).has_value());
        CY_REQUIRE(ser::resolve_against(moved_pair, schema).has_value());
        CY_REQUIRE(frame.render(moved_pair, view));
        CY_REQUIRE(frame.previous_material_transform(identity, previous));
        CY_CHECK_EQ(previous.row0[3], doctest::Approx(-view.position[0]));
        CY_REQUIRE(frame.previous_material_transform(second_identity, previous));
        CY_CHECK_EQ(previous.row0[3], doctest::Approx(4.0 - view.position[0]));

        const std::string changed = edited(kSurfaceVertexGraph, "0.25", "0.75", Occurrence::First);
        CY_REQUIRE(frame.preview(reference, changed));
        rhi::null::clear_command_log(**device);
        CY_REQUIRE(frame.render(world, view));
        CY_REQUIRE(frame.previous_material_transform(identity, previous));
        CY_CHECK_EQ(previous.row0[3], doctest::Approx(-view.position[0]));
        u32 rebuilt = 0;
        for (const auto& command : rhi::null::command_log(**device)) {
            if (command.kind == rhi::null::CommandKind::BindGraphicsPipeline &&
                std::ranges::find(standard, command.handle_bits) == standard.end() &&
                std::ranges::find(selected, command.handle_bits) == selected.end()) {
                ++rebuilt;
            }
        }
        CY_CHECK_GE(rebuilt, 2U);

        CY_REQUIRE(frame.preview(reference, kSceneInterpolantGraph));
        rhi::null::clear_command_log(**device);
        CY_REQUIRE(frame.render(world, view));
        u32 interpolated_draws = 0;
        for (const auto& command : rhi::null::command_log(**device)) {
            if (command.kind == rhi::null::CommandKind::BindGraphicsPipeline &&
                std::ranges::find(standard, command.handle_bits) == standard.end()) {
                ++interpolated_draws;
            }
        }
        CY_CHECK_GE(interpolated_draws, 2U);

        std::ifstream saved_file(std::string(CY_TEST_PROJECT) + "/" + reference);
        CY_REQUIRE(saved_file.good());
        const std::string saved(std::istreambuf_iterator<char>{saved_file}, {});
        CY_REQUIRE(frame.preview(reference, saved));
        rhi::null::clear_command_log(**device);
        CY_REQUIRE(frame.render(world, view));
        CY_CHECK_FALSE(frame.previous_material_transform(identity, previous));
        u32 restored_variants = 0;
        for (const auto& command : rhi::null::command_log(**device)) {
            if (command.kind == rhi::null::CommandKind::BindGraphicsPipeline &&
                std::ranges::find(standard, command.handle_bits) == standard.end()) {
                ++restored_variants;
            }
        }
        CY_CHECK_EQ(restored_variants, 0U);
    }
    rhi::destroy_device(allocator(), *device);
}
#endif

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
    if ((*device)->capabilities().backend() != kNativeBackend) {
        std::fprintf(stderr, "no %s device: selected '%s' because %s\n", kBackend,
                     selection.selected, selection.reason);
        CY_CHECK(selection.fell_back);
        rhi::destroy_device(allocator(), *device);
        return;
    }

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
#if defined(CY_SHADER_SLANG) && CY_SHADER_SLANG
        check_graph_displacement_matches_cpu(frame, schema, view);
        check_graph_motion_matches_cpu(**device, schema, view);
#endif
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
    if ((*device)->capabilities().backend() != rhi::BackendKind::Metal) {
        std::fprintf(stderr, "no Metal device: selected '%s' because %s\n", selection.selected,
                     selection.reason);
        CY_CHECK(selection.fell_back);
        rhi::destroy_device(allocator(), *device);
        return;
    }
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
