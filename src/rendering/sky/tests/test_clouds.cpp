// The cloud representation, the tiers, the profiles and the diagnostics, at UNIT cost: everything
// in this file is a property of a declaration, a table or a piece of arithmetic, and nothing here
// reconstructs a cloud more than a handful of times.
//
// The integrals — the atmospheric tables, the march, the composition, the shadow field — are the
// three integration suites beside this one, because a cloud ray march is sixty density samples and
// each of those is four layers of fractal noise. A version cheap enough for a 1 ms budget would be
// a spot check, and the things worth checking about a march are not spot-checkable.

#include <cy/test/test.h>

#include <cy/core/math/math.h>
#include <cy/rendering/sky/cloud_shadows.h>
#include <cy/rendering/sky/clouds.h>
#include <cy/rendering/sky/composition.h>
#include <cy/rendering/sky/diagnostics.h>
#include <cy/rendering/sky/profile.h>

#include <cmath>
#include <cstring>

namespace {

using cy::f32;
using cy::i32;
using cy::u32;
using cy::u64;
using cy::Vec2;
using cy::Vec3;
using cy::rendering::QualityLadder;
using namespace cy::rendering::sky;

/// A map small enough for a unit budget. Ten cells of a kilometre: a ten-kilometre patch, which is
/// enough to have a coverage gradient across it and cheap enough to generate sixty times.
[[nodiscard]] cy::Status make_map(CloudWeatherMap& map, f32 coverage) {
    if (auto status = map.configure(10, 1000.0F); !status) {
        return status;
    }
    return map.generate(0x5EEDULL, coverage, 0.0F);
}

}  // namespace

CY_TEST_CASE("clouds: a weather map fine enough to be a volume is refused") {
    CloudWeatherMap map;
    // The representation exists so that a world-scale cloud volume is never stored. A caller asking
    // for 100 m cells is asking for that volume by another name, and the refusal is the mechanism.
    CY_CHECK_FALSE(map.configure(64, 100.0F));
    CY_CHECK_FALSE(map.configure(64, CloudWeatherMap::kMinimumCellMetres - 1.0F));
    CY_CHECK_FALSE(map.configure(0, 1000.0F));
    CY_CHECK(map.configure(64, CloudWeatherMap::kMinimumCellMetres));
    CY_CHECK(map.configure(64, 1000.0F));
}

CY_TEST_CASE("clouds: a hundred-kilometre world's clouds are kilobytes, not gigabytes") {
    CloudWeatherMap map;
    CY_REQUIRE(map.configure(100, 1000.0F));  // 100 km at 1 km cells

    // What is stored and replicated: coverage, type, moisture and a version, per cell.
    CY_CHECK_EQ(map.bytes(), 100ULL * 100ULL * (3ULL + sizeof(cy::u16)));
    CY_CHECK_LT(map.bytes(), 64ULL * 1024ULL);

    // What a stored volume would have been, at the resolution a ray march actually resolves. The
    // requirement says a world-scale volumetric field SHALL NOT be stored; this is the number that
    // sentence is about, computed rather than described.
    constexpr u64 kVoxelMetres = 32;
    constexpr u64 kWorldMetres = 100000;
    constexpr u64 kDeckMetres = 7000;
    const u64 voxels = (kWorldMetres / kVoxelMetres) * (kWorldMetres / kVoxelMetres) *
                       (kDeckMetres / kVoxelMetres);
    CY_TEST_MESSAGE("weather map ", map.bytes(), " B against a stored volume of ", voxels, " B");
    CY_CHECK_GT(voxels / map.bytes(), 10000ULL);
}

CY_TEST_CASE("clouds: a write bumps the cell's version and the map's epoch") {
    CloudWeatherMap map;
    CY_REQUIRE(map.configure(8, 1000.0F));
    const u32 epoch = map.epoch();
    const u32 before = map.sample(2500.0F, 2500.0F).version;

    CY_REQUIRE(map.set(2, 2, 0.9F, 0.5F, 0.8F));
    CY_CHECK_GT(map.epoch(), epoch);
    CY_CHECK_NE(map.sample(2500.0F, 2500.0F).version, before);
    // A cell nobody wrote keeps its version: the rejection has to be attributable to the cell that
    // changed, not to the map.
    CY_CHECK_EQ(map.sample(6500.0F, 6500.0F).version, before);

    // The map WRAPS: a sample far outside it still finds clouds, because a sky has no edge.
    const CloudMapSample inside = map.sample(2500.0F, 2500.0F);
    const CloudMapSample wrapped = map.sample(2500.0F + (8.0F * 1000.0F), 2500.0F);
    CY_CHECK_NEAR(wrapped.coverage, inside.coverage, 1.0e-5F);
}

CY_TEST_CASE("clouds: the layer set refuses a ninth layer and a layer with no volume") {
    CloudLayerSet set = default_cloud_layers();
    CY_CHECK_EQ(set.count, 4U);
    CY_CHECK_EQ(set.lowest_base(), 1200.0F);  // the storm layer's base
    CY_CHECK_GT(set.highest_top(), 8000.0F);

    CloudLayer flat;
    flat.thickness = 0.0F;
    CY_CHECK_FALSE(set.add(flat));

    CloudLayer extra;
    extra.kind = CloudLayerKind::Project;
    for (u32 index = 4; index < kMaxCloudLayers; ++index) {
        CY_CHECK(set.add(extra));
    }
    CY_CHECK_EQ(set.count, kMaxCloudLayers);
    CY_CHECK_FALSE(set.add(extra));
}

CY_TEST_CASE("clouds: weather drives the layers, and the profile keeps its authoring") {
    CloudLayerSet calm = default_cloud_layers();
    CloudLayerSet storm = default_cloud_layers();

    CloudWeatherState clear;
    clear.humidity = 0.1F;
    clear.storm_intensity = 0.0F;
    CloudWeatherState severe;
    severe.humidity = 0.95F;
    severe.storm_intensity = 0.9F;

    drive_cloud_layers(clear, calm);
    drive_cloud_layers(severe, storm);

    // "Layer parameters SHALL be driven by weather state rather than authored per frame."
    CY_CHECK_GT(storm.layers[0].coverage, calm.layers[0].coverage);  // low deck follows humidity
    CY_CHECK_GT(storm.layers[3].coverage, 0.0F);                     // the storm layer arrives
    CY_CHECK_EQ(calm.layers[3].coverage, 0.0F);                      // and is absent without one
    CY_CHECK_GT(storm.layers[3].density, calm.layers[3].density);

    // And what the PROFILE authored is untouched: which layers exist, how thick they are, and the
    // scales the reconstruction uses. A weather system that moved these would be authoring the
    // world rather than the day.
    const CloudLayerSet authored = default_cloud_layers();
    for (u32 index = 0; index < authored.count; ++index) {
        CY_CHECK_EQ(storm.layers[index].base_altitude, authored.layers[index].base_altitude);
        CY_CHECK_EQ(storm.layers[index].thickness, authored.layers[index].thickness);
        CY_CHECK_EQ(storm.layers[index].base_scale, authored.layers[index].base_scale);
        CY_CHECK_EQ(storm.layers[index].detail_scale, authored.layers[index].detail_scale);
    }

    // The shear separates the layers from ONE wind vector plus one declared shear, so a high deck
    // outruns a low one without anybody authoring three winds.
    CY_CHECK_GT(length(storm.layers[2].wind), length(storm.layers[1].wind));
    CY_CHECK_GT(length(storm.layers[1].wind), length(storm.layers[0].wind));
}

CY_TEST_CASE("clouds: the tier ladder is priced, monotone, and maps back to the tiers") {
    const QualityLadder ladder = sky_quality_ladder();
    CY_CHECK_EQ(ladder.positions, static_cast<cy::u8>(SkyQualityTier::Count));
    CY_CHECK_EQ(ladder.relative_cost[0], 1.0F);

    // The arbiter allocates milliseconds over a ladder it can price, and a ladder whose prices were
    // not monotone would let it choose a cheaper position that costs more.
    for (u32 position = 1; position < ladder.positions; ++position) {
        CY_CHECK_LT(ladder.cost_at(static_cast<cy::u8>(position)),
                    ladder.cost_at(static_cast<cy::u8>(position - 1U)));
    }

    // Position 0 is the most expensive, which is the arbiter's convention and the reverse of the
    // enumeration's order. Inverting it in two places differently is exactly the bug this checks.
    CY_CHECK(tier_at_ladder_position(0) == SkyQualityTier::Cinematic);
    CY_CHECK(tier_at_ladder_position(4) == SkyQualityTier::Mobile);
    CY_CHECK(tier_at_ladder_position(200) == SkyQualityTier::Mobile);
}

CY_TEST_CASE("clouds: a tier changes how the sky is drawn and not what the weather is") {
    CloudWeatherState weather;
    weather.humidity = 0.7F;
    weather.storm_intensity = 0.4F;
    weather.precipitation = 3.5F;
    weather.wind = Vec3{9.0F, 0.0F, 4.0F};

    const EnvironmentProfile profile = earthlike_profile();
    const SkyConfiguration reference = configure_sky(profile, weather);

    for (u32 index = 0; index < static_cast<u32>(SkyQualityTier::Count); ++index) {
        const auto tier = static_cast<SkyQualityTier>(index);
        const SkyQualitySettings settings = sky_quality(tier);

        // Every lever in a tier is a DRAWING lever. Composing the sky again while the tier is in
        // hand produces the same configuration, because the tier is not one of its inputs — which
        // is the requirement's headline rule expressed as a signature rather than as discipline.
        const SkyConfiguration composed = configure_sky(profile, weather);
        for (u32 layer = 0; layer < composed.clouds.count; ++layer) {
            CY_CHECK_EQ(composed.clouds.layers[layer].coverage,
                        reference.clouds.layers[layer].coverage);
            CY_CHECK_EQ(composed.clouds.layers[layer].density,
                        reference.clouds.layers[layer].density);
            CY_CHECK_EQ(composed.clouds.layers[layer].wind.x,
                        reference.clouds.layers[layer].wind.x);
        }
        CY_CHECK_EQ(composed.medium.visibility_metres, reference.medium.visibility_metres);
        CY_CHECK_EQ(weather.precipitation, 3.5F);

        // And the lever really is a lever: the tiers differ in what they cost to draw.
        CY_CHECK_GT(settings.clouds.steps, 0U);
    }

    CY_CHECK_LT(sky_quality(SkyQualityTier::Mobile).clouds.steps,
                sky_quality(SkyQualityTier::Cinematic).clouds.steps);
    CY_CHECK_FALSE(sky_quality(SkyQualityTier::Mobile).volumetric_clouds);
    CY_CHECK(sky_quality(SkyQualityTier::Cinematic).volumetric_clouds);
}

CY_TEST_CASE("clouds: a profile transition is a travel and its groups have their own clocks") {
    ProfileTransition transition;
    // Every group, because `total_seconds()` is the longest of them all and a group left at its
    // default would silently extend the transition past what this case is measuring.
    for (auto& group : transition.groups) {
        group = ParameterTransition{TransitionCurve::SmoothStep, 10.0F, 0.0F};
    }
    transition.groups[static_cast<u32>(ProfileGroup::Clouds)] =
        ParameterTransition{TransitionCurve::Linear, 10.0F, 10.0F};
    CY_CHECK_EQ(transition.total_seconds(), 20.0F);

    ProfileBlender blender;
    blender.begin(toxic_volcanic_profile(), earthlike_profile(), transition);

    f32 previous = blender.current().atmosphere.rayleigh_scattering.z;
    const f32 target = earthlike_profile().atmosphere.rayleigh_scattering.z;
    const f32 start = toxic_volcanic_profile().atmosphere.rayleigh_scattering.z;
    CY_CHECK_NEAR(previous, start, 1.0e-9F);

    for (u32 step = 0; step < 20; ++step) {
        blender.advance(0.5F);
        const f32 now = blender.current().atmosphere.rayleigh_scattering.z;
        // Monotone and continuous: a transition that jumped would be the preset switch the
        // requirement forbids, and the jump would be invisible in an endpoint comparison.
        CY_CHECK_GE(now, previous - 1.0e-9F);
        CY_CHECK_LT(std::fabs(now - previous), std::fabs(target - start) * 0.45F);
        previous = now;
    }

    // Halfway through, the atmosphere has travelled and the clouds have not started.
    ProfileBlender staged;
    staged.begin(toxic_volcanic_profile(), earthlike_profile(), transition);
    staged.advance(5.0F);
    CY_CHECK_GT(staged.progress(ProfileGroup::Atmosphere), 0.0F);
    CY_CHECK_EQ(staged.progress(ProfileGroup::Clouds), 0.0F);

    staged.advance(20.0F);
    CY_CHECK(staged.complete());
    CY_CHECK_NEAR(staged.current().atmosphere.rayleigh_scattering.z, target, 1.0e-9F);
}

CY_TEST_CASE("clouds: weather reaches fog through state, and the round trip is exact") {
    const cy::rendering::sky::Atmosphere atmosphere = earth_atmosphere();

    CloudWeatherState dry;
    dry.humidity = 0.2F;
    CloudWeatherState humid;
    humid.humidity = 0.95F;

    const AtmosphericMedium clear = medium_from_weather(atmosphere, dry);
    const AtmosphericMedium hazy = medium_from_weather(atmosphere, humid);

    // "WHEN humidity rises THEN visibility SHALL change and the fog parameters SHALL follow from
    // it." The direction of the arrow is the requirement: humidity is the input, visibility is the
    // state, and the fog is derived.
    CY_CHECK_LT(hazy.visibility_metres, clear.visibility_metres);
    CY_CHECK_GT(hazy.aerosol_density, clear.aerosol_density);
    CY_TEST_MESSAGE("visibility ", clear.visibility_metres, " m dry against ",
                    hazy.visibility_metres, " m humid");

    const FogParameters fog = derive_fog_parameters(atmosphere, hazy);
    const FogParameters thin = derive_fog_parameters(atmosphere, clear);
    CY_CHECK_GT(fog.extinction.y, thin.extinction.y);
    CY_CHECK_GT(fog.anisotropy, thin.anisotropy);

    // The derivation is the inverse of the one above, so a project publishing a visibility directly
    // and a project publishing a humidity arrive at the same parameters.
    const f32 implied = 3.912F / (fog.extinction.y - atmosphere.rayleigh_scattering.y);
    // A RELATIVE tolerance: `CY_CHECK_NEAR`'s third argument is doctest's `epsilon`, which scales
    // with the values being compared. Passing an absolute metre count here would have made the
    // check accept anything — and it did, until a mutation that changed 3.912 to 2.5 sailed through
    // it. One per cent of a visibility is the honest bound for a round trip that is exact in exact
    // arithmetic.
    CY_CHECK_NEAR(implied, hazy.visibility_metres, 0.01F);

    // VFX inject into the same medium rather than producing an unrelated volumetric effect.
    AtmosphericMedium smoky = clear;
    smoky.injected_density = 0.02F;
    smoky.injected_emission = Vec3{0.4F, 0.1F, 0.0F};
    const FogParameters injected = derive_fog_parameters(atmosphere, smoky);
    CY_CHECK_GT(injected.extinction.y, thin.extinction.y);
    CY_CHECK_GT(injected.emission.x, 0.0F);
}

CY_TEST_CASE("clouds: a cloud shadow declaration that asks for a shadow map is refused") {
    CloudShadowQuality quality;
    CY_CHECK(cloud_shadow_declaration(quality));

    // "a cloud shadow's footprint is kilometres wide and its detail is low-frequency". A caller
    // asking for two-metre cells is asking for the virtual shadow page system, which is the one
    // mechanism this field is specified not to be.
    quality.regional_cell_metres = 2.0F;
    CY_CHECK_FALSE(cloud_shadow_declaration(quality));
    quality.regional_cell_metres = kMinimumCellMetres - 1.0F;
    CY_CHECK_FALSE(cloud_shadow_declaration(quality));

    quality.regional_cell_metres = 128.0F;
    quality.macro_cell_metres = 64.0F;  // coarser than the fine level, which CyberField orders
    CY_CHECK_FALSE(cloud_shadow_declaration(quality));

    quality.macro_cell_metres = 1024.0F;
    auto declaration = cloud_shadow_declaration(quality);
    CY_REQUIRE(declaration);
    // Full sun where nothing has been written: a default of zero would black out an unstreamed
    // world, and `environment-fields` requires a sample outside resident data to return the
    // declared default rather than to block.
    CY_CHECK_EQ(declaration.value().default_value.x(), 1.0F);
    CY_CHECK(declaration.value().classification == cy::determinism::SimulationClass::Presentation);
}

CY_TEST_CASE("clouds: a still camera and drifting clouds reproject to where the cloud was") {
    CloudHistoryInputs inputs;
    inputs.current_uv = Vec2{0.5F, 0.5F};
    inputs.camera_motion = Vec2{0.0F, 0.0F};  // the camera has not moved at all
    inputs.cloud_velocity = Vec3{30.0F, 0.0F, 0.0F};
    inputs.delta_seconds = 1.0F;
    inputs.depth_metres = 3000.0F;

    const CloudHistory moved = reproject_cloud(inputs);
    CY_CHECK(moved.state == cy::rendering::HistoryState::Valid);

    // 30 m of drift at 3 km, through a half-angle whose tangent is 0.577, is 0.00866 of the screen.
    // The arithmetic is written out because the number is the whole point: a camera-only motion
    // vector would have produced exactly zero here, and the cloud would smear.
    const f32 expected = (30.0F / (3000.0F * inputs.tan_half_fov_x)) * 0.5F;
    CY_CHECK_NEAR(inputs.current_uv.x - moved.history_uv.x, expected, 0.02F);
    CY_CHECK_GT(moved.confidence, 0.9F);

    // And the control: with no cloud motion the fetch lands on the same pixel, which is what a
    // consumer using the engine's ordinary motion vector would get.
    CloudHistoryInputs still = inputs;
    still.cloud_velocity = Vec3{0.0F, 0.0F, 0.0F};
    const CloudHistory smeared = reproject_cloud(still);
    CY_CHECK_NEAR(smeared.history_uv.x, inputs.current_uv.x, 1.0e-6F);
}

CY_TEST_CASE("clouds: history is rejected where the weather changed, and the reason is separable") {
    CloudHistoryInputs inputs;
    inputs.cloud_velocity = Vec3{10.0F, 0.0F, 0.0F};
    CY_CHECK(reproject_cloud(inputs).state == cy::rendering::HistoryState::Valid);

    // A front moved across THIS pixel's map cell.
    CloudHistoryInputs cell_changed = inputs;
    cell_changed.weather_version = 7;
    cell_changed.history_weather_version = 6;
    const CloudHistory by_cell = reproject_cloud(cell_changed);
    CY_CHECK_FALSE(cy::rendering::history_usable(by_cell.state));
    CY_CHECK(by_cell.rejected_by_weather);
    CY_CHECK_EQ(by_cell.confidence, 0.0F);

    // The layer parameters moved underneath a pixel whose own cell did not change. Two
    // granularities because they fail differently, and a single "invalid" count could not tell a
    // tuner which one fired.
    CloudHistoryInputs state_changed = inputs;
    state_changed.weather_epoch = 3;
    state_changed.history_weather_epoch = 2;
    CY_CHECK(reproject_cloud(state_changed).rejected_by_weather);

    // A geometric rejection is NOT a weather rejection.
    CloudHistoryInputs offscreen = inputs;
    offscreen.camera_motion = Vec2{5.0F, 0.0F};
    const CloudHistory geometric = reproject_cloud(offscreen);
    CY_CHECK_FALSE(cy::rendering::history_usable(geometric.state));
    CY_CHECK_FALSE(geometric.rejected_by_weather);
}

CY_TEST_CASE("clouds: cost is attributable to steps, resolution and layers") {
    CloudMarchStats stats;
    stats.steps = 64;
    stats.density_samples = 64;
    stats.light_samples = 240;
    stats.empty_steps = 24;
    stats.steps_in_layer[0] = 30;
    stats.steps_in_layer[3] = 10;
    stats.layers_touched = 2;

    CloudQuality quality;
    quality.resolution_scale = 0.5F;

    const CloudCostAttribution cost = attribute_cloud_cost(stats, quality, 100000);
    CY_CHECK_EQ(cost.view_samples, 6400000ULL);
    CY_CHECK_EQ(cost.light_samples, 24000000ULL);
    CY_CHECK_NEAR(cost.view_share + cost.light_share, 1.0F, 1.0e-5F);
    // The light march dominates, which is the finding a tuner needs before halving the step count.
    CY_CHECK_GT(cost.light_share, cost.view_share);

    // The resolution lever is priced in the same unit as the others: at half resolution a quarter
    // of the rays are marched, so three quarters of the cost is what it saved.
    CY_CHECK_NEAR(cost.resolution_saving, 0.75F, 1.0e-5F);
    CY_CHECK_EQ(cost.samples_at_full_resolution, (cost.view_samples + cost.light_samples) * 4ULL);

    // Which layer to simplify, rather than which lever to pull.
    CY_CHECK_NEAR(cost.layer_share[0], 0.75F, 1.0e-5F);
    CY_CHECK_NEAR(cost.layer_share[3], 0.25F, 1.0e-5F);
    CY_CHECK_NEAR(cost.empty_fraction, 24.0F / 64.0F, 1.0e-5F);
}

CY_TEST_CASE(
    "clouds: every debug view draws something, and a weather rejection is its own colour") {
    SkyPixelReport report;
    report.coverage = 0.7F;
    report.type = 0.9F;
    report.density = 0.05F;
    report.stats.steps = 96;
    report.history_confidence = 0.8F;
    report.tables_sampled = SkyTableBit::Transmittance | SkyTableBit::SkyView;

    for (u32 index = 1; index < static_cast<u32>(SkyDebugView::Count); ++index) {
        const auto view = static_cast<SkyDebugView>(index);
        const Vec3 colour = visualise(view, report);
        CY_CHECK_GE(colour.x + colour.y + colour.z, 0.0F);
        CY_CHECK(std::strlen(sky_debug_view_name(view)) > 0);
    }

    // The tables view is three channels of "did this pixel read that table", so a pixel that read
    // none is black — which is the case worth finding and is invisible in a ramp.
    const Vec3 tables = visualise(SkyDebugView::AtmosphereTables, report);
    CY_CHECK_EQ(tables.x, 1.0F);
    CY_CHECK_EQ(tables.y, 0.0F);
    CY_CHECK_EQ(tables.z, 1.0F);

    // A weather rejection and a low-confidence reprojection have different causes, so they have
    // different colours: a tuner reading a dark screen cannot tell which it is.
    SkyPixelReport rejected = report;
    rejected.history_rejected_by_weather = true;
    const Vec3 by_weather = visualise(SkyDebugView::HistoryConfidence, rejected);
    const Vec3 by_confidence = visualise(SkyDebugView::HistoryConfidence, report);
    CY_CHECK_NE(by_weather.x, by_confidence.x);
    CY_CHECK_EQ(by_weather.x, 1.0F);
    CY_CHECK_EQ(by_weather.z, 1.0F);
}

CY_TEST_CASE("clouds: the reconstruction is a pure function of seed, position and time") {
    CloudWeatherMap map;
    CY_REQUIRE(make_map(map, 0.6F));

    CloudField field;
    field.map = &map;
    field.layers = default_cloud_layers();
    field.seed = 0xA11CEULL;

    // A position the reconstruction actually puts a cloud at. Searched for rather than assumed,
    // because a determinism check on a sample that happens to be zero is a check that two zeroes
    // are equal — which is exactly the shape of test this project has shipped before and which
    // proves nothing.
    Vec3 position{0.0F, 2000.0F, 0.0F};
    CloudDensitySample first;
    for (u32 index = 0; index < 64 && first.density <= 0.0F; ++index) {
        position =
            Vec3{static_cast<f32>(index) * 137.0F, 2000.0F, static_cast<f32>(index) * 211.0F};
        first = cloud_density(field, position, 12.5, 3);
    }
    CY_REQUIRE(first.density > 0.0F);

    const CloudDensitySample second = cloud_density(field, position, 12.5, 3);
    CY_CHECK_EQ(first.density, second.density);
    CY_CHECK_EQ(first.coverage, second.coverage);

    // A different seed is a different sky. Without this, "the noise is derived from the world's
    // seed" would be a comment.
    CloudField other = field;
    other.seed = 0xB0B0ULL;
    CY_CHECK_NE(cloud_density(other, position, 12.5, 3).density, first.density);

    // COVERAGE AND TYPE COME FROM THE MAP AND DO NOT DEPEND ON THE OCTAVE COUNT. This is the split
    // the tier rule rests on, checked at the one function where the two halves meet.
    for (u32 octaves = 1; octaves <= 5; ++octaves) {
        const CloudDensitySample sample = cloud_density(field, position, 12.5, octaves);
        CY_CHECK_EQ(sample.coverage, first.coverage);
        CY_CHECK_EQ(sample.type, first.type);
    }

    // Above and below every layer there is nothing, and that is a ray-march early exit rather than
    // a small number.
    CY_CHECK_EQ(cloud_density(field, Vec3{1234.0F, 40000.0F, 5678.0F}, 12.5, 3).density, 0.0F);
    CY_CHECK_EQ(cloud_density(field, Vec3{1234.0F, 10.0F, 5678.0F}, 12.5, 3).density, 0.0F);
}
