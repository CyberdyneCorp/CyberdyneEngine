// SPDX-License-Identifier: MIT
// Lights for a moment, and which lighting path a frame takes. Unit cost, no device.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/arbiter/profiles.h>
#include <cy/rendering/lighting/many_light.h>
#include <cy/rendering/lighting/transient_lights.h>

namespace {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;
using cy::render::LightDescription;
using cy::render::LightKind;
using cy::rendering::kNoTransientLight;
using cy::rendering::LightingPath;
using cy::rendering::LightingPathInputs;
using cy::rendering::LightingPathSelector;
using cy::rendering::ManyLightSettings;
using cy::rendering::TransientFade;
using cy::rendering::TransientLight;
using cy::rendering::TransientLightDesc;
using cy::rendering::TransientLights;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

TransientLightDesc flash(f32 lifetime, TransientFade fade) noexcept {
    TransientLightDesc desc;
    desc.light.kind = LightKind::Point;
    desc.light.intensity = 1000.0F;
    desc.light.range = 4.0F;
    desc.light.transform.translation = Vec3{1.0F, 2.0F, 3.0F};
    desc.lifetime_seconds = lifetime;
    desc.fade = fade;
    return desc;
}

ManyLightSettings stochastic_available() noexcept {
    ManyLightSettings settings;
    settings.enabled = true;
    settings.denoising_available = true;
    return settings;
}

}  // namespace

CY_TEST_CASE("transient lights: a flash fades over its life and is freed when it ends") {
    TransientLights lights(allocator());
    CY_REQUIRE(lights.reserve(8).has_value());
    const auto spawned = lights.spawn(flash(0.1F, TransientFade::Quadratic));
    CY_REQUIRE(spawned.has_value());
    const TransientLight light = *spawned;
    CY_CHECK(lights.alive(light));
    cy::Array<LightDescription> frame(allocator());
    CY_CHECK_EQ(*lights.append_to(frame), 1U);
    CY_CHECK_EQ(frame[0].intensity, 1000.0F);
    CY_CHECK((frame[0].stable_id & cy::rendering::kTransientLightIdBit) != 0U);

    lights.advance(0.05F);  // half way: (1 - 0.5)^2 of the light
    frame.clear();
    CY_CHECK_EQ(*lights.append_to(frame), 1U);
    CY_CHECK_NEAR(frame[0].intensity, 250.0F, 1e-2F);

    lights.advance(0.06F);
    CY_CHECK_FALSE(lights.alive(light));
    frame.clear();
    CY_CHECK_EQ(*lights.append_to(frame), 0U);
    CY_CHECK_EQ(lights.status().expired, 1U);
    CY_CHECK_EQ(lights.status().live, 0U);
}

CY_TEST_CASE("transient lights: the envelopes start whole and end dark") {
    using cy::rendering::transient_envelope;
    CY_CHECK_EQ(transient_envelope(TransientFade::None, 0.5F, 1.0F), 1.0F);
    CY_CHECK_EQ(transient_envelope(TransientFade::None, 1.0F, 1.0F), 0.0F);
    CY_CHECK_NEAR(transient_envelope(TransientFade::Linear, 0.25F, 1.0F), 0.75F, 1e-6F);
    CY_CHECK_NEAR(transient_envelope(TransientFade::Quadratic, 0.25F, 1.0F), 0.5625F, 1e-6F);
    // No lifetime: whole until killed.
    CY_CHECK_EQ(transient_envelope(TransientFade::Quadratic, 99.0F, 0.0F), 1.0F);
}

CY_TEST_CASE("transient lights: a stale handle never answers for the light now in its slot") {
    TransientLights lights(allocator());
    CY_REQUIRE(lights.reserve(1).has_value());
    const TransientLight first = *lights.spawn(flash(0.0F, TransientFade::None));
    CY_REQUIRE(lights.kill(first).has_value());
    const TransientLight second = *lights.spawn(flash(0.0F, TransientFade::None));
    CY_CHECK_NE(first, second);
    CY_CHECK_FALSE(lights.alive(first));
    CY_CHECK(lights.alive(second));
    CY_CHECK_FALSE(lights.kill(first).has_value());
    LightDescription moved = flash(0.0F, TransientFade::None).light;
    moved.transform.translation = Vec3{9.0F, 9.0F, 9.0F};
    CY_CHECK_FALSE(lights.update(first, moved).has_value());
    CY_REQUIRE(lights.update(second, moved).has_value());
    cy::Array<LightDescription> frame(allocator());
    CY_CHECK_EQ(*lights.append_to(frame), 1U);
    CY_CHECK_EQ(frame[0].transform.translation.x, 9.0F);
    CY_CHECK_FALSE(lights.alive(kNoTransientLight));
}

CY_TEST_CASE("transient lights: a full pool refuses and counts, and nothing is grown") {
    TransientLights lights(allocator());
    CY_REQUIRE(lights.reserve(2).has_value());
    CY_CHECK(lights.spawn(flash(1.0F, TransientFade::Linear)).has_value());
    CY_CHECK(lights.spawn(flash(1.0F, TransientFade::Linear)).has_value());
    const auto refused = lights.spawn(flash(1.0F, TransientFade::Linear));
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, cy::ErrorCode::OutOfRange);
    CY_CHECK_EQ(lights.status().refused, 1U);
    CY_CHECK_EQ(lights.status().capacity, 2U);
    // A directional light and a negative life are not flashes.
    TransientLightDesc sun = flash(1.0F, TransientFade::Linear);
    sun.light.kind = LightKind::Directional;
    CY_CHECK_FALSE(lights.spawn(sun).has_value());
    CY_CHECK_FALSE(lights.spawn(flash(-1.0F, TransientFade::Linear)).has_value());
}

CY_TEST_CASE("transient lights: an area light spawns as cheaply as a point") {
    TransientLights lights(allocator());
    CY_REQUIRE(lights.reserve(4).has_value());
    TransientLightDesc panel = flash(0.5F, TransientFade::Linear);
    panel.light.kind = LightKind::Rect;
    panel.light.source_width = 0.4F;
    panel.light.source_height = 0.2F;
    CY_REQUIRE(lights.spawn(panel).has_value());
    cy::Array<LightDescription> frame(allocator());
    CY_CHECK_EQ(*lights.append_to(frame), 1U);
    CY_CHECK_EQ(frame[0].kind, LightKind::Rect);
    CY_CHECK_EQ(frame[0].source_width, 0.4F);
}

CY_TEST_CASE("lighting path: unavailable under the profile, the clustered path always") {
    LightingPathSelector selector;
    LightingPathInputs inputs;
    inputs.lights_dropped_by_bound = 500;
    inputs.local_lights = 5000;
    ManyLightSettings off;
    CY_CHECK_EQ(selector.update(off, inputs).path, LightingPath::Clustered);
    ManyLightSettings no_denoiser;
    no_denoiser.enabled = true;  // refused by validation: no denoiser
    CY_CHECK_EQ(selector.update(no_denoiser, inputs).path, LightingPath::Clustered);
}

CY_TEST_CASE(
    "lighting path: past the cluster bound or the budget it leaves at once, and returns slowly") {
    LightingPathSelector selector(10, 0.7F);
    const ManyLightSettings settings = stochastic_available();
    LightingPathInputs fits;
    fits.local_lights = 200;
    fits.clustered_ms = 1.0F;
    fits.allocation_ms = 4.0F;
    CY_CHECK_EQ(selector.update(settings, fits).path, LightingPath::Clustered);

    // Lights dropped at the bound: content missing from the picture, so it leaves THIS frame.
    LightingPathInputs crowded = fits;
    crowded.local_lights = 4000;
    crowded.lights_dropped_by_bound = 37;
    const auto left = selector.update(settings, crowded);
    CY_CHECK_EQ(left.path, LightingPath::StochasticManyLight);
    CY_CHECK(left.changed);

    // Back to two hundred lights: the predicted clustered cost fits, but only after the dwell.
    LightingPathInputs calm;
    calm.local_lights = 200;
    calm.allocation_ms = 4.0F;
    for (u32 frame = 0; frame < 9; ++frame) {
        CY_CHECK_EQ(selector.update(settings, calm).path, LightingPath::StochasticManyLight);
    }
    const auto returned = selector.update(settings, calm);
    CY_CHECK_EQ(returned.path, LightingPath::Clustered);
    CY_CHECK(returned.changed);

    // Over the arbiter's allocation without a single drop: the budget alone moves it.
    LightingPathInputs expensive = fits;
    expensive.clustered_ms = 6.5F;
    CY_CHECK_EQ(selector.update(settings, expensive).path, LightingPath::StochasticManyLight);
    // A scene hovering at the boundary does not return: 80% of the lights it left at is not 70%.
    LightingPathInputs hovering;
    hovering.local_lights = 160;
    hovering.allocation_ms = 4.0F;
    for (u32 frame = 0; frame < 30; ++frame) {
        CY_CHECK_EQ(selector.update(settings, hovering).path, LightingPath::StochasticManyLight);
    }
}

CY_TEST_CASE("lighting path: the allocation is the frame arbiter's for the forward shading") {
    cy::rendering::BudgetArbiter arbiter;
    CY_REQUIRE(cy::rendering::apply_profile(
                   cy::rendering::named_profile(cy::rendering::ProfileName::Standard), arbiter)
                   .has_value());
    const f32 granted = cy::rendering::shading_allocation_ms(arbiter);
    CY_CHECK_GT(granted, 0.0F);
    CY_CHECK_EQ(granted, arbiter.allocation_ms(cy::rendering::BudgetSubsystem::MaterialEvaluation));
    // A clustered loop costing more than that grant moves the frame to the stochastic path.
    LightingPathSelector selector;
    LightingPathInputs inputs;
    inputs.local_lights = 3000;
    inputs.clustered_ms = granted * 1.5F;
    inputs.allocation_ms = granted;
    CY_CHECK_EQ(selector.update(stochastic_available(), inputs).path,
                LightingPath::StochasticManyLight);
}
