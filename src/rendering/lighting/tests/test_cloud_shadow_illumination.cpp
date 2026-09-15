// ILLUMINATION READS THE CLOUD SHADOW FIELD, AND THE TWO ANSWERS DIFFER. M11.c task 5.2.
//
// `atmosphere-sky-and-clouds` — "Cloud shadows": the coarse world-scale field is "consumed by
// terrain, foliage, water, AND ILLUMINATION".
//
// ================================================================================================
// WHY THIS SUITE IS AN OBSERVATION AND NOT A SEARCH
// ================================================================================================
//
// `m11a:sky-field-consumed-outside-the-sky` is the criterion that exists to say whether anything
// outside `src/rendering/sky/` reads this field, and what it did was `grep -rln` for the names
// `cloud_shadow_field_id` and `CloudShadowField::sample` over `src/`. M11.a's own gate classified
// that shape — "presence-only: nothing built is run, no suite is executed and no two observations
// are compared, so it goes red for a word and not for a defect."
//
// A word is exactly what it would have taken to turn it green. This suite is what the criterion
// asks for instead: it BUILDS a real cloud shadow field with a real weather map, finds the darkest
// and the brightest ground the producer actually wrote, runs the illumination consumer at both, and
// compares the sun it gets back. Two observations, from one run, of the same code a frame calls.
//
// INTEGRATION AND NOT UNIT, because generating a cloud weather map and ray-marching 2 048 cells of
// shadow field is not a millisecond's work — `integration.render_sky_fields` pays the same cost for
// the same reason.

#include <cy/test/test.h>

#include <cy/core/math/math.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/environment/field.h>
#include <cy/environment/store.h>
#include <cy/rendering/lighting/cloud_shadow.h>
#include <cy/rendering/sky/cloud_shadows.h>
#include <cy/rendering/sky/clouds.h>
#include <cy/world/coordinates.h>

#include <cmath>

namespace {

using cy::f32;
using cy::i32;
using cy::u32;
using cy::Span;
using cy::Vec3;
using cy::world::WorldVec3d;
using cy::rendering::apply_cloud_shadows;
using cy::rendering::cloud_shadow_at;
using cy::rendering::CloudShadowIllumination;
using cy::rendering::GpuLight;
using cy::rendering::kGpuLightDirectional;
using cy::rendering::kGpuLightPoint;
using cy::rendering::sunlight_under_clouds;
using cy::rendering::sky::CloudShadowField;
using cy::rendering::sky::CloudShadowQuality;

[[nodiscard]] cy::Allocator& allocator() {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

[[nodiscard]] cy::world::PartitionConfig partition() {
    cy::world::PartitionConfig config;
    config.partition = 1;
    config.base_cell_size = 128.0F;
    config.levels = 3;
    config.level_ratio = 4;
    return config;
}

/// The same quality `integration.render_sky_fields` uses, so that the two suites are looking at one
/// field rather than at two configurations that happen to share a name.
[[nodiscard]] CloudShadowQuality test_quality() {
    CloudShadowQuality quality;
    quality.regional_cell_metres = 128.0F;
    quality.macro_cell_metres = 1024.0F;
    quality.radius_metres = 512.0F;
    quality.updates_per_second = 8.0F;
    quality.steps = 8;
    return quality;
}

/// One directional light, in the lux `GpuLight::intensity` carries. A clear midday sun is about
/// 100 000 lux and the number is the scale rather than the subject.
[[nodiscard]] GpuLight sun() {
    GpuLight light;
    light.kind = kGpuLightDirectional;
    light.intensity = 100000.0F;
    light.direction[0] = 0.3F;
    light.direction[1] = -0.8F;
    light.direction[2] = 0.2F;
    return light;
}

[[nodiscard]] GpuLight torch() {
    GpuLight light;
    light.kind = kGpuLightPoint;
    light.intensity = 800.0F;
    light.range = 12.0F;
    return light;
}

}  // namespace

CY_TEST_CASE("cloud shadow illumination: the sun under a cloud is dimmer than the sun beside it") {
    cy::environment::FieldRegistry registry(allocator());
    CloudShadowField sky;
    CY_REQUIRE(sky.attach(registry, test_quality()));
    CY_REQUIRE(CloudShadowField::declare_consumers(registry));
    cy::environment::FieldStore store(allocator(), registry, partition());

    // BROKEN CLOUD, for the reason `test_cloud_shadows.cpp` gives: an overcast sky writes a field
    // that is uniformly dark, which satisfies "the darkest cell is dark" while containing no shadow
    // at all. What a shadow IS is the difference between the lit ground and the shaded ground, and
    // this case's whole claim is that illumination can see that difference.
    cy::rendering::sky::CloudWeatherMap map;
    CY_REQUIRE(map.configure(16, 1000.0F));
    CY_REQUIRE(map.generate(0x5ADE5ULL, 0.45F, 0.2F));
    cy::rendering::sky::CloudField clouds;
    clouds.map = &map;
    clouds.layers = cy::rendering::sky::default_cloud_layers();
    cy::rendering::sky::CloudWeatherState weather;
    weather.humidity = 0.45F;
    weather.storm_intensity = 0.2F;
    cy::rendering::sky::drive_cloud_layers(weather, clouds.layers);

    const Vec3 direction = normalize(Vec3{0.3F, 0.8F, 0.2F});
    CY_REQUIRE(sky.update(store, clouds, direction, 0.0, WorldVec3d{}, 1.0F));

    // THE TWO POSITIONS ARE FOUND, NOT CHOSEN. The M10 defect this whole line of criteria descends
    // from was a case that probed twenty-five hand-picked points and read full sun at all of them
    // because that ground is genuinely in full sun. So the darkest and the brightest ground are
    // searched for THROUGH THE CONSUMER, which is also the first assertion: a consumer that
    // returned the declared default everywhere would find no darkest ground to report.
    WorldVec3d shaded{};
    WorldVec3d lit{};
    f32 darkest = 2.0F;
    f32 brightest = -1.0F;
    const auto span = static_cast<i32>(cy::environment::kTileCells);
    const f32 cell = test_quality().regional_cell_metres;
    for (i32 tile_z = -1; tile_z <= 0; ++tile_z) {
        for (i32 tile_x = -1; tile_x <= 0; ++tile_x) {
            for (i32 local_z = 0; local_z < span; ++local_z) {
                for (i32 local_x = 0; local_x < span; ++local_x) {
                    const double at_x = (static_cast<double>((tile_x * span) + local_x) + 0.5) *
                                        static_cast<double>(cell);
                    const double at_z = (static_cast<double>((tile_z * span) + local_z) + 0.5) *
                                        static_cast<double>(cell);
                    const WorldVec3d at{at_x, 0.0, at_z};
                    const f32 transmittance = cloud_shadow_at(store, at);
                    if (transmittance < darkest) {
                        darkest = transmittance;
                        shaded = at;
                    }
                    if (transmittance > brightest) {
                        brightest = transmittance;
                        lit = at;
                    }
                }
            }
        }
    }

    GpuLight lights[2] = {sun(), torch()};
    const f32 authored_sun = lights[0].intensity;
    const f32 authored_torch = lights[1].intensity;

    const CloudShadowIllumination under_cloud =
        apply_cloud_shadows(Span<GpuLight>(lights, 2), store, shaded);
    const f32 shaded_sun = lights[0].intensity;
    const f32 shaded_torch = lights[1].intensity;

    lights[0] = sun();
    lights[1] = torch();
    const CloudShadowIllumination in_the_open =
        apply_cloud_shadows(Span<GpuLight>(lights, 2), store, lit);
    const f32 lit_sun = lights[0].intensity;

    CY_TEST_MESSAGE("illumination through the cloud shadow field: sun ", authored_sun, " lux -> ",
                    shaded_sun, " lux at (", shaded.x, ", ", shaded.z, "), ", lit_sun, " lux at (",
                    lit.x, ", ", lit.z, "), transmittance ", under_cloud.transmittance, " and ",
                    in_the_open.transmittance, ", punctual lights untouched ",
                    under_cloud.punctual_lights);

    // THE TWO ANSWERS DIFFER, which is the whole claim and the thing a search for the words could
    // not establish.
    CY_CHECK_LT(shaded_sun, lit_sun);
    CY_CHECK_LT(under_cloud.transmittance, 0.99F);
    CY_CHECK_GT(in_the_open.transmittance, 0.99F);
    CY_CHECK_GT(in_the_open.transmittance - under_cloud.transmittance, 0.01F);

    // And the attenuation is the field's number rather than a second one invented here.
    CY_CHECK_NEAR(shaded_sun, authored_sun * under_cloud.transmittance, 1.0F);
    CY_CHECK_NEAR(lit_sun, authored_sun * in_the_open.transmittance, 1.0F);
    CY_CHECK_EQ(under_cloud.directional_lights, 1U);

    // A CLOUD SHADOWS THE SUN AND NOT A TORCH. A point light is under the deck with the surface it
    // lights and there is nothing between them for the field to describe; dimming it would make a
    // character's own lantern flicker when a cloud passed overhead.
    CY_CHECK_EQ(under_cloud.punctual_lights, 1U);
    CY_CHECK_NEAR(shaded_torch, authored_torch, 1e-3F);

    // The same number reached the other way round, so that `sunlight_under_clouds` and
    // `apply_cloud_shadows` cannot drift into two answers.
    CY_CHECK_NEAR(sunlight_under_clouds(sun(), store, shaded), shaded_sun, 1.0F);
    CY_CHECK_NEAR(sunlight_under_clouds(torch(), store, shaded), authored_torch, 1e-3F);
}

CY_TEST_CASE("cloud shadow illumination: an unstreamed world is in full sun, not in the dark") {
    // `environment-fields`: "A sample outside resident data SHALL return the declared default rather
    // than block." For this field the declared default is FULL SUN, and the reason is this case: a
    // default of zero would black out every part of the world the shadow field had not reached,
    // which is most of it most of the time.
    cy::environment::FieldRegistry registry(allocator());
    CloudShadowField sky;
    CY_REQUIRE(sky.attach(registry, test_quality()));
    cy::environment::FieldStore store(allocator(), registry, partition());

    GpuLight lights[1] = {sun()};
    const CloudShadowIllumination nothing_written =
        apply_cloud_shadows(Span<GpuLight>(lights, 1), store, WorldVec3d{9000000.0, 0.0, -9000000.0});
    CY_CHECK_NEAR(nothing_written.transmittance, 1.0F, 1e-4F);
    CY_CHECK_NEAR(lights[0].intensity, sun().intensity, 1.0F);
}
