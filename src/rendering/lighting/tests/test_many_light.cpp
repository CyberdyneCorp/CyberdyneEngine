// Channels, cookies and the stochastic many-light path. Task 10.3.
//
// The reservoir estimator's UNBIASEDNESS is the case worth reading: it is measured against a
// reference sum over every light, because getting the weight expression wrong does not crash and
// does not look obviously wrong — it looks like a scene that is slightly too bright in the places
// with the most lights.

#include <cy/test/test.h>

#include <cy/core/math/math.h>
#include <cy/rendering/lighting/channels.h>
#include <cy/rendering/lighting/light_functions.h>
#include <cy/rendering/lighting/many_light.h>

#include <cmath>

namespace {

using cy::f32;
using cy::u32;
using cy::Vec3;
using cy::rendering::active_lighting_path;
using cy::rendering::apply_light_function;
using cy::rendering::assign_cluster_lights;
using cy::rendering::channel_bit;
using cy::rendering::ChannelFilterStats;
using cy::rendering::ChannelMask;
using cy::rendering::channels_intersect;
using cy::rendering::cookie_filter_level;
using cy::rendering::cookie_uv;
using cy::rendering::CookieProjection;
using cy::rendering::CookieProjectionKind;
using cy::rendering::kAllChannels;
using cy::rendering::LightCandidate;
using cy::rendering::LightFunction;
using cy::rendering::lighting_path_stats;
using cy::rendering::LightingPath;
using cy::rendering::ManyLightSettings;
using cy::rendering::NamedChannel;
using cy::rendering::Reservoir;
using cy::rendering::sample_lights;
using cy::rendering::SampleStream;
using cy::rendering::validate_many_light;

}  // namespace

CY_TEST_CASE("channels: a character-only key light is not assigned to world geometry") {
    const ChannelMask characters = channel_bit(NamedChannel::Characters);
    const ChannelMask world = channel_bit(NamedChannel::World);

    const ChannelMask lights[3] = {characters, world, kAllChannels};
    const f32 importance[3] = {10.0F, 5.0F, 1.0F};
    u32 assigned[3] = {};

    const ChannelFilterStats on_world =
        assign_cluster_lights(world, lights, importance, 3, 3, assigned);
    CY_CHECK_EQ(on_world.assigned, 2U);
    CY_CHECK_EQ(on_world.rejected_by_channel, 1U);
    CY_CHECK_EQ(on_world.rejected_by_bound, 0U);
    // The character key light is index 0 and it is not in the answer.
    CY_CHECK_NE(assigned[0], 0U);
    CY_CHECK_NE(assigned[1], 0U);

    const ChannelFilterStats on_character =
        assign_cluster_lights(characters, lights, importance, 3, 3, assigned);
    CY_CHECK_EQ(on_character.assigned, 2U);
    CY_CHECK_EQ(assigned[0], 0U);  // most important first
}

CY_TEST_CASE("channels: the bound drops the same lights twice, and says which reason") {
    ChannelMask lights[6];
    f32 importance[6];
    for (u32 index = 0; index < 6; ++index) {
        lights[index] = index < 2U ? channel_bit(NamedChannel::Interior) : kAllChannels;
        importance[index] = static_cast<f32>(6U - index);
    }

    u32 first[2] = {};
    u32 second[2] = {};
    const ChannelFilterStats stats =
        assign_cluster_lights(channel_bit(NamedChannel::World), lights, importance, 6, 2, first);
    (void)assign_cluster_lights(channel_bit(NamedChannel::World), lights, importance, 6, 2, second);

    CY_CHECK_EQ(first[0], second[0]);
    CY_CHECK_EQ(first[1], second[1]);
    CY_CHECK_EQ(stats.assigned, 2U);
    // Two rejections for an artistic reason and two for a budget one, reported apart — which is the
    // difference between "my light is off" and "my scene has too many lights".
    CY_CHECK_EQ(stats.rejected_by_channel, 2U);
    CY_CHECK_EQ(stats.rejected_by_bound, 2U);
}

CY_TEST_CASE("channels: equal importance breaks on the lowest index, not on iteration order") {
    const ChannelMask lights[4] = {kAllChannels, kAllChannels, kAllChannels, kAllChannels};
    const f32 importance[4] = {1.0F, 1.0F, 1.0F, 1.0F};
    u32 assigned[2] = {};
    (void)assign_cluster_lights(kAllChannels, lights, importance, 4, 2, assigned);
    CY_CHECK_EQ(assigned[0], 0U);
    CY_CHECK_EQ(assigned[1], 1U);
    CY_CHECK(channels_intersect(kAllChannels, channel_bit(NamedChannel::Vfx)));
    CY_CHECK_FALSE(
        channels_intersect(channel_bit(NamedChannel::Vfx), channel_bit(NamedChannel::World)));
}

CY_TEST_CASE("cookies: a spot's cookie lands on the cone, and nothing behind the apex is lit") {
    CookieProjection projection;
    projection.kind = CookieProjectionKind::ConePerspective;
    projection.position = Vec3{0.0F, 0.0F, 0.0F};
    projection.forward = Vec3{0.0F, 0.0F, -1.0F};
    projection.right = Vec3{1.0F, 0.0F, 0.0F};
    projection.up = Vec3{0.0F, 1.0F, 0.0F};
    projection.cone_half_angle = 0.7853982F;  // 45 degrees, so tan is 1

    // On the axis: the cookie's centre.
    const auto centre = cookie_uv(projection, Vec3{0.0F, 0.0F, -5.0F});
    CY_CHECK(centre.inside);
    CY_CHECK_NEAR(centre.uv.x, 0.5F, 1.0e-5F);
    CY_CHECK_NEAR(centre.uv.y, 0.5F, 1.0e-5F);

    // On the cone at 45 degrees: the cookie's edge.
    const auto edge = cookie_uv(projection, Vec3{5.0F, 0.0F, -5.0F});
    CY_CHECK(edge.inside);
    CY_CHECK_NEAR(edge.uv.x, 1.0F, 1.0e-4F);

    // Behind the apex: not lit. The arithmetic is perfectly happy to mirror the pattern there,
    // which is the classic gobo bug.
    CY_CHECK_FALSE(cookie_uv(projection, Vec3{0.0F, 0.0F, 5.0F}).inside);
    // Outside a non-tiling cookie: not lit.
    CY_CHECK_FALSE(cookie_uv(projection, Vec3{50.0F, 0.0F, -5.0F}).inside);
}

// `cookies: a directional cloud cookie is the same at any altitude and costs no shadow` was
// here and is now in test_cookie_scroll.cpp, in the integration suite. Its 20,000-frame scroll
// loop measured 0.87 to 0.95 ms of CPU in the Debug configuration against a 1.00 ms unit
// budget, and the loop length IS the property. That file carries the measurement.

CY_TEST_CASE("cookies: a point light's cookie covers the whole sphere") {
    CookieProjection projection;
    projection.kind = CookieProjectionKind::CubeDirection;
    projection.position = Vec3{2.0F, 3.0F, 4.0F};
    for (Vec3 offset : {Vec3{1.0F, 0.0F, 0.0F}, Vec3{-1.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F},
                        Vec3{0.0F, -1.0F, 0.0F}}) {
        const auto sample = cookie_uv(projection, projection.position + offset);
        CY_CHECK(sample.inside);
        CY_CHECK_NEAR(length(sample.direction), 1.0F, 1.0e-4F);
        CY_CHECK_GE(sample.uv.x, -1.0e-4F);
        CY_CHECK_LE(sample.uv.x, 1.0F + 1.0e-4F);
    }
}

CY_TEST_CASE("cookies: intensity and the outside value are what a fade and a gobo are") {
    LightFunction function;
    function.intensity = 1.0F;
    function.outside_value = 0.0F;
    cy::rendering::CookieSample inside;
    inside.inside = true;
    cy::rendering::CookieSample outside;
    outside.inside = false;

    CY_CHECK_NEAR(apply_light_function(function, inside, 0.75F), 0.75F, 1.0e-6F);
    CY_CHECK_EQ(apply_light_function(function, outside, 0.75F), 0.0F);

    function.intensity = 0.0F;  // faded out without unbinding the texture
    CY_CHECK_EQ(apply_light_function(function, inside, 0.75F), 0.0F);

    // A mask that only darkens where it is drawn.
    function.intensity = 1.0F;
    function.outside_value = 1.0F;
    CY_CHECK_EQ(apply_light_function(function, outside, 0.0F), 1.0F);

    CY_CHECK_EQ(cookie_filter_level(0.5F, 1), 0.0F);
    CY_CHECK_GT(cookie_filter_level(0.5F, 9), cookie_filter_level(0.01F, 9));
}

CY_TEST_CASE("many-light: the path is off by default and refused without a denoiser") {
    ManyLightSettings settings;
    // "Clustered lighting SHALL remain the default shipping path."
    CY_CHECK_FALSE(settings.enabled);
    CY_CHECK_EQ(active_lighting_path(settings), LightingPath::Clustered);
    CY_CHECK(validate_many_light(settings));

    settings.enabled = true;
    // "This path SHALL NOT be enabled without denoising... enforced by configuration validation
    // rather than discovered visually." Refused, not silently downgraded.
    CY_CHECK_FALSE(validate_many_light(settings));
    CY_CHECK_EQ(active_lighting_path(settings), LightingPath::Clustered);

    settings.denoising_available = true;
    CY_CHECK(validate_many_light(settings));
    CY_CHECK_EQ(active_lighting_path(settings), LightingPath::StochasticManyLight);

    settings.visibility_rays_per_pixel = 0;
    CY_CHECK_FALSE(validate_many_light(settings));
}

CY_TEST_CASE("many-light: the limit that is reported is the one that applies") {
    ManyLightSettings clustered;
    const auto a = lighting_path_stats(clustered, 32, 7);
    CY_CHECK_EQ(a.path, LightingPath::Clustered);
    CY_CHECK_EQ(a.max_lights_per_cluster, 32U);
    CY_CHECK_EQ(a.lights_dropped_by_bound, 7U);
    CY_CHECK_EQ(a.candidates_per_pixel, 0U);

    ManyLightSettings stochastic;
    stochastic.enabled = true;
    stochastic.denoising_available = true;
    const auto b = lighting_path_stats(stochastic, 32, 7);
    CY_CHECK_EQ(b.path, LightingPath::StochasticManyLight);
    // The per-cluster bound does not apply, so it is ABSENT rather than merely ignored: "a light
    // limit that does not apply is more confusing than one that does".
    CY_CHECK_EQ(b.max_lights_per_cluster, 0U);
    CY_CHECK_EQ(b.lights_dropped_by_bound, 0U);
    CY_CHECK_EQ(b.candidates_per_pixel, stochastic.candidates_per_pixel);
    CY_CHECK_EQ(b.visibility_rays_per_pixel, stochastic.visibility_rays_per_pixel);
}

CY_TEST_CASE("many-light: a reservoir's sample count is capped so history stops being stubborn") {
    // Without the cap a temporal reservoir accumulates confidence forever and stops responding to
    // the world changing — a light switched off stays lit for seconds.
    Reservoir accumulated;
    accumulated.light = 3;
    accumulated.target = 1.0F;
    accumulated.weight_sum = 100.0F;
    accumulated.sample_count = 400;

    Reservoir fresh;
    fresh.light = 9;
    fresh.target = 2.0F;
    fresh.weight_sum = 4.0F;
    fresh.sample_count = 16;

    SampleStream stream(1, 1, 1);
    cy::rendering::reservoir_combine(accumulated, fresh, 2.0F, 500U, stream);
    CY_CHECK_LE(accumulated.sample_count, 500U);

    Reservoir saturated = accumulated;
    for (u32 round = 0; round < 50; ++round) {
        cy::rendering::reservoir_combine(saturated, fresh, 2.0F, 500U, stream);
    }
    CY_CHECK_EQ(saturated.sample_count, 500U);
}

CY_TEST_CASE("many-light: two runs of one frame select the same lights") {
    // A stochastic frame still has to be reproducible: the seed is a hash of (pixel, frame) and
    // nothing else, so a replay and a capture agree.
    constexpr u32 kLights = 32;
    LightCandidate candidates[kLights];
    for (u32 index = 0; index < kLights; ++index) {
        candidates[index].index = index;
        candidates[index].unshadowed = 1.0F + static_cast<f32>(index);
    }
    ManyLightSettings settings;
    settings.enabled = true;
    settings.denoising_available = true;

    for (u32 pixel = 0; pixel < 64; ++pixel) {
        SampleStream first(pixel, 5U, 11U);
        SampleStream second(pixel, 5U, 11U);
        const Reservoir a =
            sample_lights(cy::Span<const LightCandidate>(candidates, kLights), settings, first);
        const Reservoir b =
            sample_lights(cy::Span<const LightCandidate>(candidates, kLights), settings, second);
        CY_CHECK_EQ(a.light, b.light);
        CY_CHECK_EQ(a.weight_sum, b.weight_sum);
    }
}
