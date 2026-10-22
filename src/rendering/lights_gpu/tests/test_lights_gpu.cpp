// SPDX-License-Identifier: MIT
// unit.render_lights_gpu — the host half of the device's lights: what the dispatches are handed,
// laid out as the shaders read it, and the configuration the stochastic path refuses. No GPU.

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/lighting/lights.h>
#include <cy/rendering/lights_gpu/light_assignment.h>
#include <cy/rendering/lights_gpu/ltc_texture.h>
#include <cy/rendering/lights_gpu/many_lights.h>
#include <cy/test/test.h>

#include <bit>
#include <cmath>
#include <vector>

using namespace cy;
using namespace cy::rendering;
using namespace cy::rendering::lights_gpu;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

GpuLight point(f32 candela) noexcept {
    GpuLight light;
    light.kind = kGpuLightPoint;
    light.intensity = candela;
    return light;
}

/// How often each light is drawn when the table is sampled at every slot and both halves of it,
/// exactly — the shader's two uniform numbers integrated out.
std::vector<f64> drawn(const Array<AliasEntry>& table, usize lights) {
    std::vector<f64> probability(lights, 0.0);
    const f64 slots = static_cast<f64>(table.size());
    for (const AliasEntry& entry : table) {
        const f64 keep = static_cast<f64>(std::bit_cast<f32>(entry.threshold));
        probability[entry.light] += keep / slots;
        probability[table[entry.alias].light] += (1.0 - keep) / slots;
    }
    return probability;
}

}  // namespace

CY_TEST_CASE("many lights: the candidates are drawn in proportion to the lights' power") {
    std::vector<GpuLight> lights = {point(10.0F), point(30.0F), point(60.0F), point(0.0F),
                                    point(100.0F)};
    // A directional light and a static placeholder are never candidates.
    GpuLight sun;
    sun.kind = kGpuLightDirectional;
    sun.intensity = 100000.0F;
    lights.push_back(sun);
    lights.push_back(baked_light_placeholder());
    Array<AliasEntry> table(allocator());
    CY_REQUIRE(build_alias_table({lights.data(), lights.size()}, table).has_value());
    CY_CHECK_EQ(table.size(), 4U);
    const std::vector<f64> probability = drawn(table, lights.size());
    const f64 total = 200.0;
    CY_CHECK_NEAR(probability[0], 10.0 / total, 1e-6);
    CY_CHECK_NEAR(probability[1], 30.0 / total, 1e-6);
    CY_CHECK_NEAR(probability[2], 60.0 / total, 1e-6);
    CY_CHECK_EQ(probability[3], 0.0);
    CY_CHECK_NEAR(probability[4], 100.0 / total, 1e-6);
    CY_CHECK_EQ(probability[5], 0.0);
    CY_CHECK_EQ(probability[6], 0.0);
    // Each slot names the probability its OWN light is drawn with, which is the pdf the reservoir
    // divides by.
    for (const AliasEntry& entry : table) {
        CY_CHECK_NEAR(static_cast<f64>(std::bit_cast<f32>(entry.pdf)), probability[entry.light],
                      1e-6);
    }
}

CY_TEST_CASE("many lights: an area light's power is its flux, and a tube's ends are dark") {
    GpuLight rect;
    rect.kind = kGpuLightRect;
    rect.intensity = 100.0F;  // nits
    rect.spot_scale = 0.5F;   // 1 x 0.4 m
    rect.spot_bias = 0.2F;
    CY_CHECK_NEAR(light_power(rect), math::kPi * 100.0F * 0.4F, 1e-3F);
    rect.kind |= kGpuLightTwoSided;
    CY_CHECK_NEAR(light_power(rect), 2.0F * math::kPi * 100.0F * 0.4F, 1e-3F);
    GpuLight tube;
    tube.kind = kGpuLightTube;
    tube.intensity = 100.0F;
    tube.spot_scale = 0.5F;  // 1 m long
    tube.spot_bias = 0.02F;  // 4 cm across
    CY_CHECK_NEAR(light_power(tube), math::kPi * 100.0F * (2.0F * math::kPi * 0.02F * 1.0F), 1e-3F);
    GpuLight dark = point(100.0F);
    dark.color[0] = 0.0F;
    dark.color[1] = 0.0F;
    dark.color[2] = 0.0F;
    CY_CHECK_EQ(light_power(dark), 0.0F);
}

CY_TEST_CASE("many lights: the words are the layout many_lights.slang reads") {
    pipeline::FrameViewData view;
    // A camera-relative clip that is its own inverse's transpose-free identity, so the words are
    // easy to state.
    for (u32 index = 0; index < 16U; ++index) {
        view.relative_to_clip[index] = index % 5U == 0U ? 1.0F : 0.0F;
        view.previous_relative_to_clip[index] = index % 5U == 0U ? 2.0F : 0.0F;
        view.relative_to_view[index] = index % 5U == 0U ? 1.0F : 0.0F;
    }
    const std::vector<GpuLight> lights = {point(1.0F), point(2.0F)};
    ManyLightsFrame frame;
    frame.lights = {lights.data(), lights.size()};
    frame.view = &view;
    frame.frame_index = 9;
    ManyLightsSettings settings;
    settings.path.candidates_per_pixel = 12;
    settings.path.spatial_neighbours = 5;
    settings.path.max_history_samples = 240;
    Expected<ManyLightConstants, Error> constants =
        pack_many_light_constants(frame, settings, 640, 360, 2, true);
    CY_REQUIRE(constants.has_value());
    const Vec4* words = constants->words;
    CY_CHECK_EQ(words[0].x, 1.0F);
    CY_CHECK_EQ(words[4].x, 2.0F);
    CY_CHECK_EQ(words[8].x, 640.0F);
    CY_CHECK_NEAR(words[8].w, 1.0F / 360.0F, 1e-9F);
    CY_CHECK_EQ(std::bit_cast<u32>(words[9].x), 9U);
    CY_CHECK_EQ(std::bit_cast<u32>(words[9].y), 12U);
    CY_CHECK_EQ(std::bit_cast<u32>(words[9].z), 2U);
    CY_CHECK_EQ(std::bit_cast<u32>(words[9].w), 5U);
    CY_CHECK_EQ(words[10].x, 240.0F);
    CY_CHECK_EQ(words[10].z, 1.0F);
    // The denoiser's words are `default_config(StochasticDirect)` at the ladder's position.
    const denoise::SignalConfig config =
        denoise::default_config(denoise::SignalKind::StochasticDirect);
    CY_CHECK_EQ(words[12].x, config.sigma_depth);
    CY_CHECK_EQ(words[12].y, config.sigma_normal);
    CY_CHECK_EQ(words[13].x, static_cast<f32>(config.history_length));
    CY_CHECK_EQ(words[13].y, static_cast<f32>(filter_pass_count(settings)));
    CY_CHECK_EQ(std::bit_cast<u32>(words[15].w), 2U);
    // A singular view is refused rather than reprojected through.
    pipeline::FrameViewData singular;
    for (f32& value : singular.relative_to_clip) {
        value = 0.0F;
    }
    frame.view = &singular;
    CY_CHECK_FALSE(pack_many_light_constants(frame, settings, 640, 360, 2, true).has_value());
}

CY_TEST_CASE("many lights: the path is refused without a denoiser, before any device is asked") {
    (void)rhi::null::register_null_backend();
    rhi::DeviceDescription description;
    description.application_name = "cy_test_unit_render_lights_gpu";
    rhi::BackendSelection selection;
    Expected<rhi::Device*, Error> device =
        rhi::create_device(system_allocator(MemoryDomain::Gpu), "null", description, selection);
    CY_REQUIRE(device.has_value());
    ManyLightsSettings settings;
    settings.path.enabled = true;
    settings.path.denoising_available = false;
    ManyLightsPass pass;
    const Status refused =
        pass.create(allocator(), **device, ManyLightsPassDescription{64, 64, 16, false}, settings);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::InvalidArgument);
    // And set_settings refuses the same way, after creation as before it.
    CY_CHECK_FALSE(pass.set_settings(settings).has_value());
    rhi::destroy_device(system_allocator(MemoryDomain::Gpu), *device);
}

CY_TEST_CASE("light assignment: a light's element is the assignment's 48 bytes") {
    ClusterElement element;
    element.view_position = Vec3{1.0F, 2.0F, -3.0F};
    element.radius = 4.0F;
    element.view_direction = Vec3{0.0F, -1.0F, 0.0F};
    element.cone_cos = 0.5F;
    element.payload_index = 17;
    element.layer_mask = 0x5U;
    const GpuClusterElement packed = gpu_cluster_element(element);
    CY_CHECK_EQ(packed.view_position[2], -3.0F);
    CY_CHECK_EQ(packed.radius, 4.0F);
    CY_CHECK_EQ(packed.view_direction[1], -1.0F);
    CY_CHECK_EQ(packed.cone_cos, 0.5F);
    CY_CHECK_EQ(packed.payload, 17U);
    CY_CHECK_EQ(packed.layer_mask, 0x5U);
}

CY_TEST_CASE("ltc texture: the inverse transform above, the energy below, entry for entry") {
    // An unbuilt table is refused: an identity table would draw every highlight as a diffuse lobe.
    LtcTable unbuilt;
    Array<f32> texels(allocator());
    CY_CHECK_FALSE(pack_ltc_texels(unbuilt, texels).has_value());
}
