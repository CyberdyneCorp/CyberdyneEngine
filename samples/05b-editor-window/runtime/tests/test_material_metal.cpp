#include <cy/backends/rhi-metal/backend.h>
#include <cy/backends/rhi/backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/registry.h>
#include <cy/rendering/material/compiler.h>
#include <cy/rendering/material/text.h>
#include <cy/test/test.h>
#include <cy_reflect_generated_scene.h>

#include "material_runtime.h"

#include <cstring>
#include <utility>

using namespace cy;
using namespace cy::sample;
using namespace cy::sample::editor_window;

namespace {

constexpr std::string_view kMaterial = R"(
material live_tint {
    param tint : float = 0.1;
    texture albedo_map average (0.5, 0.5, 0.5, 1.0);
    attribute uv0 : float2;
    let sampled = sample(albedo_map, uv0).xyz;
    surface = diffuse(sampled * (tint, 1.0 - tint, 1.0));
    opacity = 1.0;
}
)";

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Gpu);
}

struct Device {
    rhi::BackendSelection selection;
    rhi::Device* handle = nullptr;

    Device() {
        (void)rhi::metal::register_metal_backend();
        rhi::DeviceDescription description;
        description.application_name = "smoke.editor_material_metal";
        description.enable_validation = true;
        auto created =
            rhi::create_device(allocator(), rhi::metal::kMetalBackendName, description, selection);
        CY_REQUIRE(created.has_value());
        handle = *created;
    }

    ~Device() {
        if (handle != nullptr) {
            (void)handle->wait_idle();
            rhi::destroy_device(allocator(), handle);
        }
    }
};

rendering::material::CompiledMaterial compile_material() {
    rendering::material::ParseDiagnostic diagnostics(allocator());
    auto module = rendering::material::parse_material(kMaterial, allocator(), diagnostics);
    CY_REQUIRE(module.has_value());
    rendering::material::CompileOptions options;
    options.derive_family = false;
    options.derive_tiers = false;
    auto compiled = rendering::material::compile_material(*module, options, allocator());
    CY_REQUIRE(compiled.has_value());
    return std::move(*compiled);
}

bool images_differ(Span<const u32> left, Span<const u32> right) {
    if (left.size() != right.size()) {
        return true;
    }
    for (usize index = 0; index < left.size(); ++index) {
        if (left[index] != right[index]) {
            return true;
        }
    }
    return false;
}

Array<u32> copy_image(Span<const u32> source) {
    Array<u32> copy(allocator());
    CY_REQUIRE(copy.append(source));
    return copy;
}

}  // namespace

CY_TEST_CASE("compiled materials bind, update and leave the exact Metal viewport object") {
    Device device;
    first_light::Scene scene(allocator());
    first_light::SceneDescription scene_description;
    scene_description.box_count = kWorldCapacity;
    CY_REQUIRE(scene.build(scene_description));

    reflect::TypeRegistry types;
    CY_REQUIRE(reflect::register_scene_types(types));
    WorldView world(allocator());
    CY_REQUIRE(world.open(CY_SAMPLE_PROJECT, "worlds/city.cyworld", types));
    CY_REQUIRE_EQ(world.present(scene), 3U);

    first_light::Renderer renderer(allocator(), *device.handle);
    first_light::RendererOptions renderer_options;
    renderer_options.width = 160;
    renderer_options.height = 90;
    renderer_options.readback = true;
    CY_REQUIRE(renderer.prepare(scene, renderer_options));

    const first_light::Camera camera = world.framing(scene);
    auto baseline_frame = renderer.render(scene, camera);
    CY_REQUIRE(baseline_frame.has_value());
    Array<u32> baseline = copy_image(renderer.color_texels());

    MetalMaterialRuntime runtime(allocator(), renderer, world);
    auto compiled = compile_material();
    const u64 artefact = compiled.cook_key();
    Status published = runtime.publish(artefact, compiled);
    if (!published) {
        CY_TEST_MESSAGE("publish failed: ", published.error().message);
    }
    CY_REQUIRE(published);
    constexpr u64 preview = (u64{1} << 32U) | 1U;
    CY_REQUIRE(runtime.create(preview));

    const u64 entity = world.identity_of(1);
    CY_REQUIRE_NE(entity, 0U);
    editor::MaterialPreviewTarget target;
    std::memcpy(target.entity, &entity, sizeof(entity));
    target.material_slot = 0;
    CY_REQUIRE(runtime.reload(preview, artefact, {&target, 1}));
    auto applied_frame = renderer.render(scene, camera);
    CY_REQUIRE(applied_frame.has_value());
    Array<u32> applied = copy_image(renderer.color_texels());
    CY_CHECK(images_differ(baseline.span(), applied.span()));

    const f64 tint = 0.9;
    const editor::MaterialParameterUpdate update{
        rendering::parameter_id("tint"), 3, {reinterpret_cast<const u8*>(&tint), sizeof(tint)}};
    CY_REQUIRE(runtime.update(preview, artefact, update));
    auto updated_frame = renderer.render(scene, camera);
    CY_REQUIRE(updated_frame.has_value());
    CY_CHECK(images_differ(applied.span(), renderer.color_texels()));
    Array<u32> updated = copy_image(renderer.color_texels());

    constexpr std::string_view checker = "builtin://checker";
    u8 texture_value[4 + checker.size()] = {};
    const u32 texture_length = static_cast<u32>(checker.size());
    std::memcpy(texture_value, &texture_length, sizeof(texture_length));
    std::memcpy(texture_value + 4, checker.data(), checker.size());
    const editor::MaterialParameterUpdate texture_update{rendering::parameter_id("albedo_map"), 5,
                                                         texture_value};
    CY_REQUIRE(runtime.update(preview, artefact, texture_update));
    const u8 wrong_type = 1;
    const editor::MaterialParameterUpdate mismatched_update{
        rendering::parameter_id("tint"), 1, {&wrong_type, 1}};
    CY_CHECK_FALSE(runtime.update(preview, artefact, mismatched_update));

    editor::MaterialPreviewTarget invalid = target;
    invalid.material_slot = 1;
    CY_CHECK_FALSE(runtime.reload(preview, artefact, {&invalid, 1}));
    auto rejected_frame = renderer.render(scene, camera);
    CY_REQUIRE(rejected_frame.has_value());
    CY_CHECK_FALSE(images_differ(updated.span(), renderer.color_texels()));
    Array<u32> rejected = copy_image(renderer.color_texels());

    CY_REQUIRE(runtime.destroy(preview));
    auto destroyed_frame = renderer.render(scene, camera);
    CY_REQUIRE(destroyed_frame.has_value());
    CY_CHECK(images_differ(rejected.span(), renderer.color_texels()));

    CY_REQUIRE(runtime.create(preview + (u64{1} << 32U)));
    CY_REQUIRE(runtime.destroy(preview + (u64{1} << 32U)));

    editor::MaterialService service(allocator(), &runtime);
    CyServiceSession session = nullptr;
    CY_REQUIRE_EQ(service.open(&session), CY_RESULT_OK);
    const CyServiceRequest cancelled_request{sizeof(CyServiceRequest), 1,       41,
                                             "material.compile",       nullptr, 0};
    CY_REQUIRE_EQ(service.submit(session, cancelled_request), CY_RESULT_OK);
    CY_REQUIRE_EQ(service.cancel(session, 41), CY_RESULT_OK);
    CyServiceEvent event{};
    bool present = false;
    CY_REQUIRE_EQ(service.poll(session, event, present), CY_RESULT_OK);
    CY_REQUIRE(present);
    CY_CHECK_EQ(event.kind, static_cast<u32>(CY_SERVICE_EVENT_CANCELLED));

    const CyServiceRequest create_request{sizeof(CyServiceRequest), 1,       42,
                                          "preview.create",         nullptr, 0};
    CY_REQUIRE_EQ(service.submit(session, create_request), CY_RESULT_OK);
    CY_REQUIRE_EQ(service.poll(session, event, present), CY_RESULT_OK);
    CY_REQUIRE_EQ(event.payload_size, sizeof(u64));
    u64 disconnected_preview = 0;
    std::memcpy(&disconnected_preview, event.payload, sizeof(disconnected_preview));
    service.close(session);
    CY_CHECK_FALSE(runtime.reload(disconnected_preview, artefact, {&target, 1}));
}
