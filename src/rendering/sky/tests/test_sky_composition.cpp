// THE CLOUD RECONSTRUCTION, THE MARCH, AND THE SKY AS ONE COMPOSED THING.
//
// Integration, because a cloud ray march is sixty density samples and each of those is up to four
// layers of fractal noise — and because the claims worth making here are about what a march
// PRODUCES over a range of weather, not about one sample of it.

#include <cy/test/test.h>

#include <cy/core/math/math.h>
#include <cy/rendering/sky/clouds.h>
#include <cy/rendering/sky/composition.h>
#include <cy/rendering/sky/diagnostics.h>
#include <cy/rendering/sky/profile.h>
#include <cy/rendering/sky/tables.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;
using namespace cy::rendering::sky;

[[nodiscard]] Vec3 direction_at(f32 elevation_degrees, f32 azimuth_degrees) {
    const f32 elevation = elevation_degrees * cy::math::kDegToRad;
    const f32 azimuth = azimuth_degrees * cy::math::kDegToRad;
    return Vec3{std::cos(elevation) * std::cos(azimuth), std::sin(elevation),
                std::cos(elevation) * std::sin(azimuth)};
}

/// A sky with weather in it, built once per case. The map is 32 cells of a kilometre — a
/// thirty-two-kilometre patch, which is several weather systems across and cheap to generate.
struct Fixture {
    Atmosphere atmosphere = earth_atmosphere();
    AtmosphereTables tables;
    CloudWeatherMap map;
    CloudField field;
    CelestialState celestial;
    PlanetaryView view;

    [[nodiscard]] cy::Status build(f32 coverage, f32 storminess, f32 sun_elevation) {
        if (auto status = tables.configure(SkyTableQuality::Low); !status) {
            return status;
        }
        if (auto built = tables.build(atmosphere); !built) {
            return cy::fail(built.error().code, built.error().message);
        }
        if (auto status = map.configure(32, 1000.0F); !status) {
            return status;
        }
        if (auto status = map.generate(0xC10DULL, coverage, storminess); !status) {
            return status;
        }
        field.map = &map;
        field.layers = default_cloud_layers();
        CloudWeatherState weather;
        weather.humidity = coverage;
        weather.storm_intensity = storminess;
        drive_cloud_layers(weather, field.layers);
        field.seed = 0xC10DULL;

        celestial.sun.direction = direction_at(sun_elevation, 20.0F);
        celestial.star_visibility = sun_elevation > 0.0F ? 0.0F : 1.0F;
        view = planetary_view(atmosphere, cy::world::WorldVec3d{4000.0, 120.0, 4000.0});
        return {};
    }

    [[nodiscard]] SkyCompositionInputs inputs(const CloudQuality& quality) const {
        SkyCompositionInputs composed;
        composed.atmosphere = &atmosphere;
        composed.tables = &tables;
        composed.celestial = &celestial;
        composed.clouds = &field;
        composed.view = view;
        composed.cloud_quality = quality;
        composed.time_seconds = 42.0;
        return composed;
    }
};

/// A quality cheap enough to run many times in one case, and still a volumetric march.
[[nodiscard]] CloudQuality probe_quality() {
    CloudQuality quality;
    quality.steps = 32;
    quality.light_steps = 4;
    quality.octaves = 2;
    return quality;
}

}  // namespace

CY_TEST_CASE("sky composition: coverage drives density, and an empty map makes no cloud") {
    Fixture clear;
    CY_REQUIRE(clear.build(0.05F, 0.0F, 40.0F));
    Fixture overcast;
    CY_REQUIRE(overcast.build(0.95F, 0.0F, 40.0F));

    f32 clear_total = 0.0F;
    f32 overcast_total = 0.0F;
    u32 samples = 0;
    for (u32 x = 0; x < 12; ++x) {
        for (u32 z = 0; z < 12; ++z) {
            const Vec3 position{static_cast<f32>(x) * 1700.0F, 2000.0F,
                                static_cast<f32>(z) * 1700.0F};
            clear_total += cloud_density(clear.field, position, 0.0, 3).density;
            overcast_total += cloud_density(overcast.field, position, 0.0, 3).density;
            ++samples;
        }
    }
    CY_TEST_MESSAGE("mean density: clear ", clear_total / static_cast<f32>(samples), ", overcast ",
                    overcast_total / static_cast<f32>(samples));
    CY_CHECK_GT(overcast_total, 0.0F);
    CY_CHECK_LT(clear_total, overcast_total * 0.1F);

    // A map with nothing in it produces nothing: the coverage is the gate, and if it were not the
    // density would be noise with a coverage-shaped bias on it.
    CloudWeatherMap empty;
    CY_REQUIRE(empty.configure(8, 1000.0F));
    for (cy::i32 x = 0; x < 8; ++x) {
        for (cy::i32 z = 0; z < 8; ++z) {
            CY_REQUIRE(empty.set(x, z, 0.0F, 0.0F, 0.0F));
        }
    }
    CloudField nothing = clear.field;
    nothing.map = &empty;
    for (u32 index = 0; index < 32; ++index) {
        const Vec3 position{static_cast<f32>(index) * 311.0F, 1800.0F,
                            static_cast<f32>(index) * 173.0F};
        CY_CHECK_EQ(cloud_density(nothing, position, 0.0, 3).density, 0.0F);
    }
}

CY_TEST_CASE("sky composition: a cloud march is lit, bounded, and attributes its cost") {
    Fixture sky;
    CY_REQUIRE(sky.build(0.8F, 0.3F, 35.0F));

    const CloudQuality quality = probe_quality();
    const Vec3 sun_light = sun_illuminance_tabulated(
        sky.atmosphere, sky.tables, sky.view.planet_relative, sky.celestial.sun.direction);
    const Vec3 ambient{200.0F, 220.0F, 260.0F};

    CloudMarchResult lit;
    u32 found = 0;
    for (u32 index = 0; index < 24 && found == 0; ++index) {
        const Vec3 direction =
            direction_at(25.0F + static_cast<f32>(index), 15.0F * static_cast<f32>(index));
        const CloudMarchResult result =
            march_clouds(sky.atmosphere, sky.tables, sky.field, sky.view.world_position, direction,
                         sky.celestial.sun.direction, sun_light, ambient, 0.0, quality);
        if (result.transmittance < 0.9F) {
            lit = result;
            ++found;
        }
    }
    CY_REQUIRE_EQ(found, 1U);

    // Energy: a march can only remove light and add what it scattered. A transmittance above one or
    // a negative radiance is a sign-flipped integration, and both look plausible in a picture.
    CY_CHECK_GT(lit.transmittance, 0.0F);
    CY_CHECK_LE(lit.transmittance, 1.0F);
    CY_CHECK_GT(lit.scattering.y, 0.0F);
    CY_CHECK_LT(lit.scattering.y, sun_light.y);
    CY_CHECK_GT(lit.half_transmittance_depth, 0.0F);
    CY_CHECK_LT(lit.dominant_layer, kMaxCloudLayers);

    // The stats are the cost attribution's raw material, and they have to be counted rather than
    // modelled: `light_samples` is the product of two levers, and it is the one that dominates.
    CY_CHECK_EQ(lit.stats.density_samples, lit.stats.steps);
    CY_CHECK_GT(lit.stats.light_samples, 0U);
    CY_CHECK_GE(lit.stats.layers_touched, 1U);

    const CloudCostAttribution cost = attribute_cloud_cost(lit.stats, quality, 100000);
    CY_TEST_MESSAGE("march: ", lit.stats.steps, " steps, ", lit.stats.light_samples,
                    " light samples, ", lit.stats.empty_steps, " empty; light share ",
                    cost.light_share * 100.0F, "%");
    CY_CHECK_GT(cost.light_share, 0.0F);
    f32 layer_total = 0.0F;
    for (const f32 share : cost.layer_share) {
        layer_total += share;
    }
    CY_CHECK_NEAR(layer_total, 1.0F, 1.0e-4F);

    // THE LAYER LEVER IS A LEVER. A mobile tier draws the low deck and nothing else, and it does
    // that by DISABLING the other layers rather than by truncating a loop — so the slab the march
    // runs through shrinks too, and the eight kilometres of empty air under the cirrus deck are not
    // stepped through at all. Without this the tier would cost the same and look different, which
    // is the wrong half of the trade.
    CloudQuality one_layer = quality;
    one_layer.max_layers = 1;
    const Vec3 down_low = direction_at(35.0F, 0.0F);
    const CloudMarchResult all_layers =
        march_clouds(sky.atmosphere, sky.tables, sky.field, sky.view.world_position, down_low,
                     sky.celestial.sun.direction, sun_light, ambient, 0.0, quality);
    const CloudMarchResult low_only =
        march_clouds(sky.atmosphere, sky.tables, sky.field, sky.view.world_position, down_low,
                     sky.celestial.sun.direction, sun_light, ambient, 0.0, one_layer);
    CY_TEST_MESSAGE("marched distance: ", all_layers.stats.marched_metres, " m over ",
                    all_layers.stats.layers_touched, " layers against ",
                    low_only.stats.marched_metres, " m over ", low_only.stats.layers_touched);
    CY_CHECK_LT(low_only.stats.marched_metres, all_layers.stats.marched_metres);
    for (u32 layer = 1; layer < kMaxCloudLayers; ++layer) {
        CY_CHECK_EQ(low_only.stats.steps_in_layer[layer], 0U);
    }

    // A DARKER CLOUD IS DARKER ON ITS UNDERSIDE. Self-shadowing is the most expensive thing in a
    // cloud renderer, so it is the first thing a budget takes and the first thing to check is real:
    // with the light march off, the same cloud is uniformly lit and brighter.
    CloudQuality unshadowed = quality;
    unshadowed.light_steps = 0;
    const Vec3 direction = direction_at(25.0F, 0.0F);
    const CloudMarchResult with_shadow =
        march_clouds(sky.atmosphere, sky.tables, sky.field, sky.view.world_position, direction,
                     sky.celestial.sun.direction, sun_light, ambient, 0.0, quality);
    const CloudMarchResult without =
        march_clouds(sky.atmosphere, sky.tables, sky.field, sky.view.world_position, direction,
                     sky.celestial.sun.direction, sun_light, ambient, 0.0, unshadowed);
    CY_CHECK_GE(without.scattering.y, with_shadow.scattering.y);
}

CY_TEST_CASE("sky composition: the same state drives every tier") {
    Fixture sky;
    CY_REQUIRE(sky.build(0.8F, 0.4F, 30.0F));

    const u64 bytes = sky.map.bytes();
    const u32 epoch = sky.map.epoch();
    const CloudLayerSet layers = sky.field.layers;

    Vec3 mobile_radiance{0.0F, 0.0F, 0.0F};
    Vec3 cinematic_radiance{0.0F, 0.0F, 0.0F};

    for (u32 index = 0; index < static_cast<u32>(SkyQualityTier::Count); ++index) {
        const auto tier = static_cast<SkyQualityTier>(index);
        CloudQuality quality = sky_quality(tier).clouds;
        // The shipping step counts are for a renderer, not for a test: the property under test is
        // that the STATE does not move, and the octave and layer levers are what could move it.
        quality.steps = cy::math::min(quality.steps, 24U);
        quality.light_steps = cy::math::min(quality.light_steps, 3U);

        const SkyCompositionInputs inputs = sky.inputs(quality);
        const Vec3 direction = direction_at(28.0F, 40.0F);
        const SkyCompositionSample composed = compose_sky(inputs, direction);

        // THE WEATHER IS UNTOUCHED. Not "unchanged by this call" — unchanged across every tier,
        // which is the sentence the capability leads with.
        CY_CHECK_EQ(sky.map.bytes(), bytes);
        CY_CHECK_EQ(sky.map.epoch(), epoch);
        for (u32 layer = 0; layer < layers.count; ++layer) {
            CY_CHECK_EQ(sky.field.layers.layers[layer].coverage, layers.layers[layer].coverage);
            CY_CHECK_EQ(sky.field.layers.layers[layer].density, layers.layers[layer].density);
        }

        // And the map's own answer — the coverage and type a gameplay system would read — is
        // identical at every tier, because it is not a function of the octave count.
        const Vec3 probe{5200.0F, 2100.0F, 3100.0F};
        const CloudDensitySample reference = cloud_density(sky.field, probe, 42.0, 3);
        const CloudDensitySample at_tier = cloud_density(sky.field, probe, 42.0, quality.octaves);
        CY_CHECK_EQ(at_tier.coverage, reference.coverage);
        CY_CHECK_EQ(at_tier.type, reference.type);

        // "WHEN the sky is rendered THEN it SHALL sample tables rather than integrating the
        // atmosphere per pixel" — reported per sample, so a composition that quietly stopped
        // reading the table it was handed is visible rather than merely slower.
        CY_CHECK_FALSE(composed.from_table);
        CY_CHECK(has_table(explain_sky_pixel(inputs, direction, nullptr).tables_sampled,
                           SkyTableBit::Transmittance));

        if (tier == SkyQualityTier::Mobile) {
            mobile_radiance = composed.radiance;
        }
        if (tier == SkyQualityTier::Cinematic) {
            cinematic_radiance = composed.radiance;
        }
    }

    // The tiers differ in what they DRAW. A test that only checked the state would pass against a
    // renderer whose tiers were all the same picture, which is the other way to satisfy the
    // requirement's words and none of its intent.
    CY_TEST_MESSAGE("mobile ", mobile_radiance.y, " nits against cinematic ", cinematic_radiance.y);
    CY_CHECK_NE(mobile_radiance.y, cinematic_radiance.y);
}

CY_TEST_CASE("sky composition: when cloud cover thickens, the light the scene gets changes") {
    Fixture thin;
    CY_REQUIRE(thin.build(0.15F, 0.0F, 45.0F));
    Fixture thick;
    CY_REQUIRE(thick.build(0.95F, 0.6F, 45.0F));

    const CloudQuality quality = probe_quality();
    const SkyLighting light = compose_sky_lighting(thin.inputs(quality), 12);
    const SkyLighting heavy = compose_sky_lighting(thick.inputs(quality), 12);

    // "WHEN cloud cover thickens THEN the radiance and irradiance the illumination system consumes
    // SHALL change accordingly." Both halves: the sky the ambient term integrates, and the
    // directional light the sun becomes.
    CY_TEST_MESSAGE("cloud transmittance: thin ", light.cloud_transmittance, ", thick ",
                    heavy.cloud_transmittance, "; sun ", light.sun_illuminance.y, " against ",
                    heavy.sun_illuminance.y, " lux");
    CY_CHECK_LT(heavy.cloud_transmittance, light.cloud_transmittance);
    CY_CHECK_LT(heavy.sun_illuminance.y, light.sun_illuminance.y);

    const Vec3 up{0.0F, 1.0F, 0.0F};
    CY_CHECK_GT(light.irradiance.irradiance(up).y, 0.0F);
    CY_CHECK_NE(heavy.irradiance.irradiance(up).y, light.irradiance.irradiance(up).y);

    // The gradient `rendering-global-illumination` consumes moves with it. A sky term that did not
    // would light a scene as if the overcast were not there, which is the specific failure the
    // scenario is about.
    CY_CHECK_NE(heavy.gradient.zenith.y, light.gradient.zenith.y);
}

CY_TEST_CASE("sky composition: stars are one path, whatever the source") {
    StarField procedural;
    CY_REQUIRE(generate_stars(procedural, 0xDEADBEEFULL, 400));
    CY_CHECK_EQ(procedural.stars.size(), 400U);
    CY_CHECK(procedural.source == StarSource::Procedural);

    // Deterministic: two machines with one seed see one sky, which matters the moment a screenshot
    // is compared. The engine's own `RandomStream` is what makes it so, not a generator of this
    // module's own.
    StarField again;
    CY_REQUIRE(generate_stars(again, 0xDEADBEEFULL, 400));
    for (u32 index = 0; index < 400; ++index) {
        CY_CHECK_EQ(procedural.stars[index].direction.x, again.stars[index].direction.x);
        CY_CHECK_EQ(procedural.stars[index].illuminance, again.stars[index].illuminance);
    }
    StarField other;
    CY_REQUIRE(generate_stars(other, 0xF00DULL, 400));
    CY_CHECK_NE(other.stars[7].direction.x, procedural.stars[7].direction.x);

    // FAINT STARS VASTLY OUTNUMBER BRIGHT ONES, and the shape of that is the assertion. A uniform
    // draw over illuminance gives a sky of equally bright dots, which is the single most
    // recognisable way a procedural night sky goes wrong — and it would pass any check that only
    // asked for a spread. The two counts below separate the two distributions: under the fifth
    // power a majority of stars sit in the bottom twentieth of the range and about one in fifty is
    // near the top, where a uniform draw would give one in twenty and one in ten.
    f32 brightest = 0.0F;
    for (const Star& star : procedural.stars.span()) {
        brightest = cy::math::max(brightest, star.illuminance);
    }
    u32 faint = 0;
    u32 near_top = 0;
    for (const Star& star : procedural.stars.span()) {
        if (star.illuminance < brightest * 0.05F) {
            ++faint;
        }
        if (star.illuminance > brightest * 0.9F) {
            ++near_top;
        }
    }
    CY_TEST_MESSAGE(faint, " of 400 stars are in the faintest twentieth of the range and ",
                    near_top, " are within a tenth of the brightest");
    CY_CHECK_GT(faint, 200U);
    CY_CHECK_LT(near_top, 32U);
    CY_CHECK_GT(near_top, 0U);

    // THE DIRECTIONS ARE UNIFORM OVER THE SPHERE, which means the COSINE of the polar angle is
    // drawn uniformly and not the angle itself. Drawing the angle puts two thirds of the stars in
    // the half of the sky nearest the poles instead of one half, and the result is two visible
    // clusters overhead and underfoot that nobody can name but everybody notices.
    u32 near_poles = 0;
    for (const Star& star : procedural.stars.span()) {
        if (std::fabs(star.direction.y) > 0.5F) {
            ++near_poles;
        }
    }
    const f32 polar_fraction = static_cast<f32>(near_poles) / 400.0F;
    const f32 polar_percent = polar_fraction * 100.0F;
    CY_TEST_MESSAGE("stars within sixty degrees of a pole: ", polar_percent,
                    "% (uniform over the sphere is 50; a uniformly drawn ANGLE gives 67)");
    CY_CHECK_GT(polar_fraction, 0.44F);
    CY_CHECK_LT(polar_fraction, 0.56F);

    // The SOURCE is a content decision and not a code path: an authored catalogue composes through
    // the same function, and a source with nothing in it is silence rather than a branch.
    StarField catalogue(cy::current_allocator());
    catalogue.source = StarSource::Catalogue;
    Star sirius;
    sirius.direction = normalize(Vec3{0.3F, 0.8F, 0.5F});
    sirius.illuminance = 1.0e-5F;
    CY_REQUIRE(catalogue.stars.push_back(sirius));
    CY_CHECK_GT(star_radiance(catalogue, sirius.direction, 1.0F, 0.01F).y, 0.0F);
    CY_CHECK_EQ(star_radiance(catalogue, -sirius.direction, 1.0F, 0.01F).y, 0.0F);

    // And in daylight they are gone, through `star_visibility` — the value M7's celestial model
    // computed and nothing consumed, because the sky composition consumes it and there was none.
    CY_CHECK_EQ(star_radiance(catalogue, sirius.direction, 0.0F, 0.01F).y, 0.0F);

    StarField none;
    none.source = StarSource::None;
    CY_CHECK_EQ(star_radiance(none, sirius.direction, 1.0F, 0.01F).y, 0.0F);
}

CY_TEST_CASE("sky composition: the filtered radiance map is a specular sky, not a second one") {
    Fixture sky;
    CY_REQUIRE(sky.build(0.5F, 0.0F, 50.0F));

    SkyCompositionInputs inputs = sky.inputs(probe_quality());
    inputs.clouds = nullptr;  // the map is built from the sky; the clouds are measured separately

    SkyRadianceMap map;
    // THIRTY-TWO, and the number matters. Levels are half the resolution of the one above with a
    // floor of four texels, so a base of sixteen puts the last three levels all at 4x4 and the
    // prefilter has nothing left to resolve: every roughness above a half returns the same four
    // texels and the chain looks smooth for the wrong reason. A base of 32 gives 32, 16, 8, 4, 4.
    CY_REQUIRE(map.configure(32));
    SkyRadianceMap::Sampler sampler;
    sampler.context = &inputs;
    sampler.function = [](void* context, Vec3 direction) {
        return compose_sky(*static_cast<const SkyCompositionInputs*>(context), direction).radiance;
    };
    CY_REQUIRE(map.build(sampler));
    CY_CHECK(map.built());
    CY_CHECK_GT(map.texels(), 256ULL);

    // Level 0 is the sky. A mirror reflection must see what the background sees, or a chrome sphere
    // and the sky behind it disagree.
    const Vec3 up{0.0F, 1.0F, 0.0F};
    const Vec3 sharp = map.sample(up, 0.0F);
    const Vec3 direct = compose_sky(inputs, up).radiance;
    CY_TEST_MESSAGE("mirror level ", sharp.y, " nits against the sky's ", direct.y);
    CY_CHECK_LT(std::fabs(sharp.y - direct.y) / cy::math::max(direct.y, 1.0e-6F), 0.35F);

    // AND ROUGHER IS SMOOTHER. The property that makes a prefiltered map worth having is that the
    // variance falls with roughness; a chain that only got darker would pass an energy check and
    // still shimmer. The sweep covers eight azimuths as well as the elevations, because a single
    // azimuthal slice of an octahedral map crosses few enough texels that a chain which was not
    // filtered at all still looks monotone on it.
    f32 sharp_spread = 0.0F;
    f32 previous_spread = 1.0e9F;
    for (const f32 roughness : {0.0F, 0.25F, 0.5F, 0.75F, 1.0F}) {
        f32 lowest = 1.0e9F;
        f32 highest = -1.0e9F;
        for (u32 index = 0; index < 48; ++index) {
            const f32 elevation = ((static_cast<f32>(index) / 47.0F) * 180.0F) - 90.0F;
            for (u32 turn = 0; turn < 8; ++turn) {
                const Vec3 value =
                    map.sample(direction_at(elevation, static_cast<f32>(turn) * 45.0F), roughness);
                lowest = cy::math::min(lowest, value.y);
                highest = cy::math::max(highest, value.y);
            }
        }
        const f32 spread = highest - lowest;
        CY_CHECK_LE(spread, previous_spread * 1.02F);
        if (roughness == 0.0F) {
            sharp_spread = spread;
        }
        previous_spread = spread;
    }
    // And it is not a rounding: the roughest level is a nearly uniform sky, which is what a fully
    // rough lobe integrates to. A chain whose last level still carried a third of the sharp sky's
    // range would be a chain that was resized rather than filtered.
    CY_TEST_MESSAGE("radiance map spread: ", sharp_spread, " nits at roughness 0, ",
                    previous_spread, " at roughness 1");
    CY_CHECK_GT(sharp_spread, 0.0F);
    CY_CHECK_LT(previous_spread, sharp_spread * 0.25F);
}

CY_TEST_CASE(
    "sky composition: a profile composes with a weather preset rather than duplicating it") {
    const EnvironmentProfile earth = earthlike_profile();
    const EnvironmentProfile desert = dusty_desert_profile();

    CloudWeatherState storm;
    storm.humidity = 0.9F;
    storm.storm_intensity = 0.8F;
    storm.precipitation = 12.0F;
    CloudWeatherState calm;
    calm.humidity = 0.2F;

    // ONE weather across TWO profiles: two atmospheres, one day.
    const SkyConfiguration earth_storm = configure_sky(earth, storm);
    const SkyConfiguration desert_storm = configure_sky(desert, storm);
    CY_CHECK_NE(earth_storm.atmosphere.rayleigh_scattering.z,
                desert_storm.atmosphere.rayleigh_scattering.z);
    CY_CHECK_EQ(earth_storm.clouds.layers[3].coverage, desert_storm.clouds.layers[3].coverage);

    // TWO weathers across ONE profile: one atmosphere, two days.
    const SkyConfiguration earth_calm = configure_sky(earth, calm);
    CY_CHECK_EQ(earth_calm.atmosphere.rayleigh_scattering.z,
                earth_storm.atmosphere.rayleigh_scattering.z);
    CY_CHECK_LT(earth_calm.clouds.layers[0].coverage, earth_storm.clouds.layers[0].coverage);
    CY_CHECK_GT(earth_calm.medium.visibility_metres, earth_storm.medium.visibility_metres);

    // The profile is a WORLD: which layers it has and how thick they are survive every weather,
    // and a project that wanted a rainy variant of a planet does not have to copy the planet.
    CY_CHECK_EQ(earth_calm.clouds.layers[0].base_altitude,
                earth_storm.clouds.layers[0].base_altitude);
    CY_CHECK_FALSE(desert_storm.clouds.layers[0].enabled);  // a thin dusty world has no low deck
}

CY_TEST_CASE("sky composition: a world is terraformed, and its sky follows") {
    const Atmosphere toxic = toxic_volcanic_profile().atmosphere;
    const Atmosphere breathable = earthlike_profile().atmosphere;
    CY_CHECK_NE(toxic.ozone_absorption.z, breathable.ozone_absorption.z);

    // Successive stages are points on ONE path, so a project that ships five of them and a project
    // that ships fifty get the same world at the same fraction. A table of hand-written stages
    // would have gaps between its rows, and the gaps are where a transition lives.
    Atmosphere previous = terraforming_profile(0, 8).atmosphere;
    CY_CHECK_NEAR(previous.mie_scattering, toxic.mie_scattering, 1.0e-9F);
    for (u32 stage = 1; stage <= 8; ++stage) {
        const EnvironmentProfile at_stage = terraforming_profile(stage, 8);
        CY_CHECK_LT(at_stage.atmosphere.mie_scattering, previous.mie_scattering);
        previous = at_stage.atmosphere;
    }
    CY_CHECK_NEAR(previous.mie_scattering, breathable.mie_scattering, 1.0e-9F);

    // And the sky that comes out of it is a different sky, from the coefficients alone.
    Fixture sky;
    CY_REQUIRE(sky.build(0.4F, 0.0F, 40.0F));
    const Vec3 horizon = direction_at(5.0F, 90.0F);

    AtmosphereTables toxic_tables;
    CY_REQUIRE(toxic_tables.configure(SkyTableQuality::Low));
    CY_REQUIRE(toxic_tables.build(toxic));
    const Vec3 toxic_sky = sky_radiance_tabulated(toxic, toxic_tables, ground_position(toxic, 0.0F),
                                                  horizon, sky.celestial.sun.direction, 24);
    const Vec3 earth_sky =
        sky_radiance_tabulated(sky.atmosphere, sky.tables, ground_position(sky.atmosphere, 0.0F),
                               horizon, sky.celestial.sun.direction, 24);
    const f32 toxic_ratio = toxic_sky.z / cy::math::max(toxic_sky.y, 1.0e-9F);
    const f32 earth_ratio = earth_sky.z / cy::math::max(earth_sky.y, 1.0e-9F);
    CY_TEST_MESSAGE("blue/green at the horizon: toxic ", toxic_ratio, ", earthlike ", earth_ratio);
    CY_CHECK_LT(toxic_ratio, earth_ratio);
}

CY_TEST_CASE("sky composition: a pixel can be asked what determined it") {
    Fixture sky;
    CY_REQUIRE(sky.build(0.85F, 0.4F, 30.0F));
    const SkyCompositionInputs inputs = sky.inputs(probe_quality());

    SkyPixelReport report;
    u32 found = 0;
    for (u32 index = 0; index < 36 && found == 0; ++index) {
        const Vec3 direction =
            direction_at(20.0F + (static_cast<f32>(index) * 0.7F), 10.0F * static_cast<f32>(index));
        const SkyPixelReport candidate = explain_sky_pixel(inputs, direction, nullptr);
        if (candidate.cloud_depth_metres > 0.0F) {
            report = candidate;
            ++found;
        }
    }
    CY_REQUIRE_EQ(found, 1U);

    // "For any pixel of sky or cloud, the tooling SHALL be able to report what determined it:
    // coverage, type, layer, the weather source, and the tables sampled." Five answers, and the
    // layer is a NAME rather than an index, because an index is not an answer to "which layer".
    CY_CHECK_GT(report.coverage, 0.0F);
    CY_CHECK_GE(report.type, 0.0F);
    CY_CHECK_LT(report.dominant_layer, kMaxCloudLayers);
    CY_CHECK(std::strlen(report.dominant_layer_name) > 0);
    CY_CHECK(std::strcmp(report.dominant_layer_kind, "none") != 0);
    CY_CHECK_EQ(report.weather_cell_metres, 1000.0F);
    CY_CHECK(has_table(report.tables_sampled, SkyTableBit::Transmittance));
    CY_CHECK(has_table(report.tables_sampled, SkyTableBit::MultipleScattering));
    // No sky view was handed to this composition, so the pixel says so — and a composition given
    // one says the opposite. The bit is what separates "the table is built" from "the table is
    // read", which are not the same claim and fail differently.
    CY_CHECK_FALSE(has_table(report.tables_sampled, SkyTableBit::SkyView));

    // THE CELL IT NAMES IS THE ADVECTED ONE. The wind has carried the air that is now over this
    // pixel from somewhere else, and the cell a tuner has to edit is where that air CAME FROM — not
    // the one under the pixel. Ten minutes of a ten-metre-a-second wind is six cells of a
    // kilometre, so the same ray at two times must name cells that are several apart; a report that
    // used the unadvected position would name almost the same cell at both.
    SkyCompositionInputs later = inputs;
    later.time_seconds = 600.0;
    const SkyPixelReport drifted = explain_sky_pixel(later, report.direction, nullptr);
    CY_TEST_MESSAGE("weather cell at t=42 is (", report.weather_cell_x, ", ", report.weather_cell_z,
                    "); at t=600 it is (", drifted.weather_cell_x, ", ", drifted.weather_cell_z,
                    ")");
    CY_CHECK_GE(std::abs(drifted.weather_cell_x - report.weather_cell_x), 3);

    IncrementalSkyView view;
    CY_REQUIRE(view.configure(SkyTableQuality::Low));
    CY_REQUIRE(view.update_tabulated(sky.atmosphere, sky.tables, sky.view.planet_relative,
                                     sky.celestial.sun.direction, 0));
    SkyCompositionInputs tabulated = inputs;
    tabulated.sky_view = &view;
    const SkyPixelReport from_table = explain_sky_pixel(tabulated, report.direction, nullptr);
    CY_CHECK(has_table(from_table.tables_sampled, SkyTableBit::SkyView));
    CY_CHECK_GT(report.stats.steps, 0U);
    CY_TEST_MESSAGE("pixel: layer index ", report.dominant_layer, ", coverage ", report.coverage,
                    ", type ", report.type, ", weather cell (", report.weather_cell_x, ", ",
                    report.weather_cell_z, ") version ", report.weather_version);

    // The elements sum to the pixel. A report whose parts did not add up to the whole would be a
    // report a tuner cannot use to attribute a too-bright sky to one of them.
    const Vec3 parts =
        ((report.atmosphere_radiance + report.stellar + report.stars + report.aurora) *
         report.cloud_transmittance) +
        report.clouds;
    CY_CHECK_NEAR(parts.y, report.radiance.y, 1.0e-3F);
}
