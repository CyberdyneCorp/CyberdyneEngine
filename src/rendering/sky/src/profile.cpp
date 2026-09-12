// Quality tiers, the priced ladder, the named profiles, and the transition between two of them.

#include <cy/rendering/sky/profile.h>

#include <cy/core/math/math.h>

#include <cmath>

namespace cy::rendering::sky {
namespace {

/// Blend two cloud layer sets.
///
/// BY INDEX, and the sets must describe the same world for the result to mean anything: layer 0 of
/// a desert profile and layer 0 of an Earth-like one are both "the low deck". Where the counts
/// differ, the extra layers fade in or out through their COVERAGE rather than appearing at full
/// strength halfway through a terraforming — a world that grows a storm layer should grow it, not
/// switch it on.
[[nodiscard]] CloudLayerSet blend_layers(const CloudLayerSet& from, const CloudLayerSet& to,
                                         f32 t) noexcept {
    CloudLayerSet result;
    result.count = math::max(from.count, to.count);
    for (u32 index = 0; index < result.count; ++index) {
        const bool has_from = index < from.count;
        const bool has_to = index < to.count;
        const CloudLayer& a = has_from ? from.layers[index] : to.layers[index];
        const CloudLayer& b = has_to ? to.layers[index] : from.layers[index];

        CloudLayer layer = b;
        layer.name = has_to ? b.name : a.name;
        layer.kind = has_to ? b.kind : a.kind;
        layer.base_altitude = math::lerp(a.base_altitude, b.base_altitude, t);
        layer.thickness = math::lerp(a.thickness, b.thickness, t);
        layer.density = math::lerp(a.density, b.density, t);
        layer.type = math::lerp(a.type, b.type, t);
        layer.base_scale = math::lerp(a.base_scale, b.base_scale, t);
        layer.detail_scale = math::lerp(a.detail_scale, b.detail_scale, t);
        layer.erosion = math::lerp(a.erosion, b.erosion, t);
        layer.wind = lerp(a.wind, b.wind, t);
        layer.coverage = math::lerp(has_from ? a.coverage : 0.0F, has_to ? b.coverage : 0.0F, t);
        layer.enabled = a.enabled || b.enabled;
        result.layers[index] = layer;
    }
    return result;
}

[[nodiscard]] CelestialModel blend_celestial(const CelestialModel& a, const CelestialModel& b,
                                             f32 t) noexcept {
    CelestialModel model;
    model.latitude_degrees = math::lerp(a.latitude_degrees, b.latitude_degrees, t);
    model.longitude_degrees = math::lerp(a.longitude_degrees, b.longitude_degrees, t);
    model.axial_tilt_degrees = math::lerp(a.axial_tilt_degrees, b.axial_tilt_degrees, t);
    model.azimuth_offset_degrees =
        math::lerp(a.azimuth_offset_degrees, b.azimuth_offset_degrees, t);
    model.moon_period_days = math::lerp(a.moon_period_days, b.moon_period_days, t);
    model.moon_phase_offset = math::lerp(a.moon_phase_offset, b.moon_phase_offset, t);
    return model;
}

[[nodiscard]] Aurora blend_aurora(const Aurora& a, const Aurora& b, f32 t) noexcept {
    Aurora aurora;
    aurora.enabled = a.enabled || b.enabled;
    aurora.altitude = math::lerp(a.altitude, b.altitude, t);
    aurora.oval_latitude = math::lerp(a.oval_latitude, b.oval_latitude, t);
    aurora.oval_width = math::lerp(a.oval_width, b.oval_width, t);
    aurora.colour = lerp(a.colour, b.colour, t);
    // An aurora that is off at one end fades rather than blinks: its intensity travels from zero.
    aurora.intensity =
        math::lerp(a.enabled ? a.intensity : 0.0F, b.enabled ? b.intensity : 0.0F, t);
    return aurora;
}

[[nodiscard]] StylisedDistance blend_stylisation(const StylisedDistance& a,
                                                 const StylisedDistance& b, f32 t) noexcept {
    StylisedDistance style;
    style.strength = math::lerp(a.strength, b.strength, t);
    style.half_distance = math::lerp(a.half_distance, b.half_distance, t);
    style.tint = lerp(a.tint, b.tint, t);
    return style;
}

[[nodiscard]] f32 curve_value(TransitionCurve curve, f32 t) noexcept {
    const f32 clamped = math::saturate(t);
    switch (curve) {
        case TransitionCurve::Immediate:
            return 1.0F;
        case TransitionCurve::Linear:
            return clamped;
        case TransitionCurve::SmoothStep:
            return clamped * clamped * (3.0F - (2.0F * clamped));
        case TransitionCurve::Hold:
            return clamped >= 1.0F ? 1.0F : 0.0F;
        case TransitionCurve::Count:
            break;
    }
    return clamped;
}

}  // namespace

// ================================================================================================
// Quality tiers
// ================================================================================================

const char* sky_quality_tier_name(SkyQualityTier tier) noexcept {
    switch (tier) {
        case SkyQualityTier::Mobile:
            return "mobile";
        case SkyQualityTier::Low:
            return "low";
        case SkyQualityTier::Medium:
            return "medium";
        case SkyQualityTier::High:
            return "high";
        case SkyQualityTier::Cinematic:
            return "cinematic";
        case SkyQualityTier::Count:
            break;
    }
    return "unknown";
}

SkyQualitySettings sky_quality(SkyQualityTier tier) noexcept {
    SkyQualitySettings settings;
    switch (tier) {
        case SkyQualityTier::Mobile:
            settings.tables = SkyTableQuality::Low;
            settings.sky_view_row_budget = 2;
            settings.radiance_map_resolution = 16;
            settings.clouds = CloudQuality{16, 2, 1, 1, 0.5F, false};
            settings.cloud_shadows = CloudShadowQuality{512.0F, 4096.0F, 2.0F, 2048.0F, 6};
            settings.volumetric_clouds = false;
            settings.temporal_reconstruction = false;
            break;
        case SkyQualityTier::Low:
            settings.tables = SkyTableQuality::Low;
            settings.sky_view_row_budget = 3;
            settings.radiance_map_resolution = 16;
            settings.clouds = CloudQuality{32, 3, 2, 2, 0.5F, false};
            settings.cloud_shadows = CloudShadowQuality{256.0F, 2048.0F, 4.0F, 3072.0F, 8};
            break;
        case SkyQualityTier::Medium:
            settings.tables = SkyTableQuality::Medium;
            settings.sky_view_row_budget = 4;
            settings.radiance_map_resolution = 32;
            settings.clouds = CloudQuality{64, 6, 3, 4, 0.5F, true};
            settings.cloud_shadows = CloudShadowQuality{128.0F, 1024.0F, 8.0F, 4096.0F, 12};
            break;
        case SkyQualityTier::High:
            settings.tables = SkyTableQuality::High;
            settings.sky_view_row_budget = 8;
            settings.radiance_map_resolution = 48;
            settings.clouds = CloudQuality{96, 8, 4, kMaxCloudLayers, 0.75F, true};
            settings.cloud_shadows = CloudShadowQuality{128.0F, 1024.0F, 12.0F, 6144.0F, 16};
            break;
        case SkyQualityTier::Cinematic:
        case SkyQualityTier::Count:
            settings.tables = SkyTableQuality::Cinematic;
            settings.sky_view_row_budget = 16;
            settings.radiance_map_resolution = 64;
            settings.clouds = CloudQuality{192, 12, 5, kMaxCloudLayers, 1.0F, true};
            settings.cloud_shadows = CloudShadowQuality{64.0F, 512.0F, 24.0F, 8192.0F, 24};
            break;
    }
    return settings;
}

QualityLadder sky_quality_ladder() noexcept {
    QualityLadder ladder;
    ladder.positions = static_cast<u8>(SkyQualityTier::Count);
    // Position 0 is `Cinematic` and costs 1 by the arbiter's own construction. The rest are the
    // ratio of (steps x light steps x resolution scale squared) against it, which is what dominates
    // a cloud renderer's cost; the table lookups and the radiance map are a rounding error beside
    // it and are deliberately not modelled here. These are DECLARED prices, not measurements — see
    // the header.
    ladder.relative_cost[0] = 1.0F;    // cinematic: 192 steps, 12 light steps, full resolution
    ladder.relative_cost[1] = 0.28F;   // high:       96 steps,  8 light steps, 0.75 resolution
    ladder.relative_cost[2] = 0.09F;   // medium:     64 steps,  6 light steps, 0.5 resolution
    ladder.relative_cost[3] = 0.023F;  // low:        32 steps,  3 light steps, 0.5 resolution
    ladder.relative_cost[4] = 0.006F;  // mobile:     no volumetric march at all
    return ladder;
}

SkyQualityTier tier_at_ladder_position(u8 position) noexcept {
    // Position 0 is the most expensive, which is the arbiter's convention — reduction walks
    // upward — and it is the reverse of the enumeration's own order. Inverting it here once is what
    // stops every call site inverting it differently.
    const u32 last = static_cast<u32>(SkyQualityTier::Count) - 1U;
    const u32 clamped = math::min(static_cast<u32>(position), last);
    return static_cast<SkyQualityTier>(last - clamped);
}

// ================================================================================================
// The named profiles
// ================================================================================================

EnvironmentProfile earthlike_profile() noexcept {
    EnvironmentProfile profile;
    profile.name = "earthlike";
    profile.atmosphere = earth_atmosphere();
    profile.celestial = CelestialModel{};
    profile.time = TimeOfDay{};
    profile.clouds = default_cloud_layers();
    profile.star_source = StarSource::Procedural;
    profile.star_count = 2400;
    profile.aurora.enabled = false;
    return profile;
}

EnvironmentProfile dusty_desert_profile() noexcept {
    EnvironmentProfile profile;
    profile.name = "dusty-desert";
    profile.atmosphere = thin_dusty_atmosphere();
    profile.celestial.latitude_degrees = 18.0F;
    profile.celestial.axial_tilt_degrees = 25.2F;  // Mars-like
    profile.time.seconds_per_day = 1800.0F;
    profile.clouds = default_cloud_layers();
    // A thin dusty atmosphere holds very little water. The layers still EXIST — a project can drive
    // them if its story calls for it — but they are thin and high, and the low deck is gone.
    profile.clouds.layers[0].enabled = false;
    profile.clouds.layers[1].density = 0.006F;
    profile.clouds.layers[1].base_altitude = 9000.0F;
    profile.clouds.layers[2].density = 0.004F;
    profile.clouds.layers[2].base_altitude = 16000.0F;
    profile.clouds.layers[3].enabled = false;
    profile.star_source = StarSource::Procedural;
    profile.star_count = 3200;
    // A thin atmosphere scatters little, so its night sky is far brighter than Earth's.
    profile.star_intensity = 2.5F;
    return profile;
}

EnvironmentProfile toxic_volcanic_profile() noexcept {
    EnvironmentProfile profile;
    profile.name = "toxic-volcanic";
    Atmosphere atmosphere = earth_atmosphere();
    // A dense sulphurous atmosphere: heavy Mie, strongly absorbing, and a Rayleigh term shifted so
    // that what survives the column is yellow-green rather than blue. Every one of these is a
    // coefficient, which is the requirement's own point — the sky is not tinted, the air is
    // different.
    atmosphere.rayleigh_scattering = Vec3{9.0e-6F, 16.0e-6F, 8.0e-6F};
    atmosphere.rayleigh_scale_height = 12000.0F;
    atmosphere.mie_scattering = 32.0e-6F;
    atmosphere.mie_extinction = 52.0e-6F;
    atmosphere.mie_anisotropy = 0.72F;
    atmosphere.mie_scale_height = 4000.0F;
    atmosphere.ozone_absorption = Vec3{2.2e-6F, 0.9e-6F, 4.4e-6F};
    atmosphere.ground_albedo = Vec3{0.06F, 0.05F, 0.04F};
    atmosphere.stellar_illuminance = 118000.0F;
    profile.name = "toxic-volcanic";
    profile.atmosphere = atmosphere;
    profile.celestial.latitude_degrees = 8.0F;
    profile.time.seconds_per_day = 2400.0F;
    profile.clouds = default_cloud_layers();
    profile.clouds.layers[0].density = 0.13F;
    profile.clouds.layers[0].base_altitude = 900.0F;
    profile.clouds.layers[0].thickness = 3200.0F;
    profile.clouds.layers[1].density = 0.09F;
    profile.star_source = StarSource::Procedural;
    profile.star_count = 800;
    // Nothing gets through that column at night.
    profile.star_intensity = 0.05F;
    profile.aurora.enabled = false;
    return profile;
}

EnvironmentProfile terraforming_profile(u32 stage, u32 stages) noexcept {
    const f32 t =
        stages > 0 ? math::saturate(static_cast<f32>(stage) / static_cast<f32>(stages)) : 1.0F;
    f32 weights[kProfileGroupCount];
    for (f32& weight : weights) {
        weight = t;
    }
    EnvironmentProfile profile =
        blend_profiles(toxic_volcanic_profile(), earthlike_profile(), weights);
    profile.name = "terraforming";
    return profile;
}

// ================================================================================================
// Transitions
// ================================================================================================

const char* transition_curve_name(TransitionCurve curve) noexcept {
    switch (curve) {
        case TransitionCurve::Immediate:
            return "immediate";
        case TransitionCurve::Linear:
            return "linear";
        case TransitionCurve::SmoothStep:
            return "smoothstep";
        case TransitionCurve::Hold:
            return "hold";
        case TransitionCurve::Count:
            break;
    }
    return "unknown";
}

const char* profile_group_name(ProfileGroup group) noexcept {
    switch (group) {
        case ProfileGroup::Atmosphere:
            return "atmosphere";
        case ProfileGroup::Celestial:
            return "celestial";
        case ProfileGroup::Clouds:
            return "clouds";
        case ProfileGroup::Stars:
            return "stars";
        case ProfileGroup::Aurora:
            return "aurora";
        case ProfileGroup::Stylisation:
            return "stylisation";
        case ProfileGroup::Count:
            break;
    }
    return "unknown";
}

f32 ProfileTransition::total_seconds() const noexcept {
    f32 longest = 0.0F;
    for (const ParameterTransition& group : groups) {
        longest = math::max(longest, group.delay_seconds + group.duration_seconds);
    }
    return longest;
}

EnvironmentProfile blend_profiles(const EnvironmentProfile& from, const EnvironmentProfile& to,
                                  const f32 (&weights)[kProfileGroupCount]) noexcept {
    EnvironmentProfile result = to;
    const f32 atmosphere_t = math::saturate(weights[static_cast<u32>(ProfileGroup::Atmosphere)]);
    const f32 celestial_t = math::saturate(weights[static_cast<u32>(ProfileGroup::Celestial)]);
    const f32 clouds_t = math::saturate(weights[static_cast<u32>(ProfileGroup::Clouds)]);
    const f32 stars_t = math::saturate(weights[static_cast<u32>(ProfileGroup::Stars)]);
    const f32 aurora_t = math::saturate(weights[static_cast<u32>(ProfileGroup::Aurora)]);
    const f32 style_t = math::saturate(weights[static_cast<u32>(ProfileGroup::Stylisation)]);

    result.name = atmosphere_t >= 1.0F ? to.name : from.name;
    result.atmosphere = blend_atmospheres(from.atmosphere, to.atmosphere, atmosphere_t);
    result.celestial = blend_celestial(from.celestial, to.celestial, celestial_t);
    // THE CLOCK DOES NOT BLEND. A day's length is a discrete property of the world and a day that
    // is 1 200 seconds long at one end and 1 800 at the other would, halfway through, be a day
    // nobody authored — and the sun's position is an integral of that rate, so it would also jump.
    // The target's clock arrives with the target's atmosphere.
    result.time = atmosphere_t >= 1.0F ? to.time : from.time;
    result.time.fraction = to.time.fraction;
    result.clouds = blend_layers(from.clouds, to.clouds, clouds_t);
    result.star_source = stars_t >= 0.5F ? to.star_source : from.star_source;
    result.star_count = stars_t >= 0.5F ? to.star_count : from.star_count;
    result.star_intensity = math::lerp(from.star_intensity, to.star_intensity, stars_t);
    result.aurora = blend_aurora(from.aurora, to.aurora, aurora_t);
    result.stylised_distance =
        blend_stylisation(from.stylised_distance, to.stylised_distance, style_t);
    return result;
}

void ProfileBlender::begin(const EnvironmentProfile& from, const EnvironmentProfile& to,
                           const ProfileTransition& transition) noexcept {
    // Restarting from the CURRENT blend rather than from `from` when one is already running: a
    // second event during a terraforming stage must not snap the sky back to where the first
    // started. `active_` is what distinguishes the two cases.
    source_ = active_ ? current_ : from;
    target_ = to;
    current_ = source_;
    transition_ = transition;
    elapsed_ = 0.0F;
    active_ = true;
    rebuild();
}

void ProfileBlender::advance(f32 delta_seconds) noexcept {
    if (!active_) {
        return;
    }
    elapsed_ += math::max(delta_seconds, 0.0F);
    rebuild();
    if (complete()) {
        active_ = false;
        current_ = target_;
    }
}

bool ProfileBlender::complete() const noexcept {
    return elapsed_ >= transition_.total_seconds();
}

f32 ProfileBlender::progress(ProfileGroup group) const noexcept {
    const u32 index = math::min(static_cast<u32>(group), kProfileGroupCount - 1U);
    const ParameterTransition& parameters = transition_.groups[index];
    const f32 after_delay = elapsed_ - parameters.delay_seconds;
    if (after_delay <= 0.0F) {
        return 0.0F;
    }
    const f32 duration = math::max(parameters.duration_seconds, 1.0e-4F);
    return curve_value(parameters.curve, after_delay / duration);
}

void ProfileBlender::rebuild() noexcept {
    f32 weights[kProfileGroupCount];
    for (u32 index = 0; index < kProfileGroupCount; ++index) {
        weights[index] = progress(static_cast<ProfileGroup>(index));
    }
    current_ = blend_profiles(source_, target_, weights);
}

// ================================================================================================
// Composing a profile with a weather preset
// ================================================================================================

SkyConfiguration configure_sky(const EnvironmentProfile& profile,
                               const CloudWeatherState& weather) noexcept {
    SkyConfiguration configuration;
    configuration.atmosphere = profile.atmosphere;
    configuration.celestial = profile.celestial;
    configuration.time = profile.time;
    configuration.clouds = profile.clouds;
    configuration.aurora = profile.aurora;
    configuration.stylised_distance = profile.stylised_distance;
    configuration.star_source = profile.star_source;
    configuration.star_intensity = profile.star_intensity;

    // THE SEAM. The profile said which layers the world has; the weather says what they are doing.
    // One function, applied here and nowhere else, so that a project cannot end up with two
    // definitions of "what does a storm do to the low deck".
    drive_cloud_layers(weather, configuration.clouds);
    configuration.medium = medium_from_weather(profile.atmosphere, weather);
    return configuration;
}

}  // namespace cy::rendering::sky
