// The pixel report, the cost attribution, and the debug views.

#include <cy/rendering/sky/diagnostics.h>

#include <cy/core/math/math.h>

#include "internal.h"

#include <cmath>

namespace cy::rendering::sky {
namespace {

/// A viridis-like ramp: perceptually monotone, and readable by a reader who cannot distinguish red
/// from green. A rainbow ramp puts its sharpest perceptual edge in the middle of the range, which
/// is exactly where a tuner is trying to read a gradient.
[[nodiscard]] Vec3 ramp(f32 value) noexcept {
    const f32 t = math::saturate(value);
    const Vec3 low{0.267F, 0.005F, 0.329F};
    const Vec3 mid{0.128F, 0.567F, 0.551F};
    const Vec3 high{0.993F, 0.906F, 0.144F};
    return t < 0.5F ? lerp(low, mid, t * 2.0F) : lerp(mid, high, (t - 0.5F) * 2.0F);
}

}  // namespace

const char* sky_debug_view_name(SkyDebugView view) noexcept {
    switch (view) {
        case SkyDebugView::None:
            return "none";
        case SkyDebugView::CloudCoverage:
            return "cloud-coverage";
        case SkyDebugView::CloudType:
            return "cloud-type";
        case SkyDebugView::RayMarchSteps:
            return "ray-march-steps";
        case SkyDebugView::HistoryConfidence:
            return "history-confidence";
        case SkyDebugView::CloudShadowField:
            return "cloud-shadow-field";
        case SkyDebugView::CloudDensitySlice:
            return "cloud-density-slice";
        case SkyDebugView::AtmosphereTables:
            return "atmosphere-tables";
        case SkyDebugView::CelestialState:
            return "celestial-state";
        case SkyDebugView::Count:
            break;
    }
    return "unknown";
}

TableDiagnostics table_diagnostics(const AtmosphereTables& tables,
                                   const IncrementalSkyView& sky_view,
                                   f32 seconds_observed) noexcept {
    TableDiagnostics diagnostics;
    diagnostics.transmittance_rebuilds = tables.transmittance.stats().full_rebuilds;
    diagnostics.transmittance_directions = tables.transmittance.stats().directions_integrated;
    diagnostics.multiple_scattering_rebuilds = tables.multiple_scattering.stats().full_rebuilds;
    diagnostics.multiple_scattering_directions =
        tables.multiple_scattering.stats().directions_integrated;

    const IncrementalSkyStats& stats = sky_view.stats();
    diagnostics.sky_view_full_rebuilds = stats.full_rebuilds;
    diagnostics.sky_view_incremental_updates = stats.incremental_updates;
    diagnostics.sky_view_rows_rebuilt = stats.rows_rebuilt;
    diagnostics.sky_view_directions = stats.directions_integrated;
    diagnostics.worst_row_staleness = stats.worst_row_staleness;

    const u32 updates = stats.full_rebuilds + stats.incremental_updates;
    if (seconds_observed > 0.0F) {
        diagnostics.sky_view_updates_per_second = static_cast<f32>(updates) / seconds_observed;
        diagnostics.sky_view_rows_per_second =
            static_cast<f32>(stats.rows_rebuilt) / seconds_observed;
    }
    const u32 rows = math::max(sky_view.rows(), 1U);
    if (updates > 0) {
        diagnostics.mean_rebuild_fraction = static_cast<f32>(stats.rows_rebuilt) /
                                            (static_cast<f32>(updates) * static_cast<f32>(rows));
    }
    return diagnostics;
}

CelestialDiagnostics celestial_diagnostics(const TimeOfDay& time,
                                           const CelestialState& state) noexcept {
    CelestialDiagnostics diagnostics;
    diagnostics.sun_direction = state.sun.direction;
    diagnostics.sun_elevation_degrees =
        std::asin(math::clamp(state.sun.direction.y, -1.0F, 1.0F)) * math::kRadToDeg;
    // Azimuth measured from +X towards +Z and wrapped into [0, 360), so that a reader comparing two
    // reports does not have to work out whether -170 and 190 are the same direction.
    f32 azimuth = std::atan2(state.sun.direction.z, state.sun.direction.x) * math::kRadToDeg;
    if (azimuth < 0.0F) {
        azimuth += 360.0F;
    }
    diagnostics.sun_azimuth_degrees = azimuth;
    diagnostics.moon_direction = state.moon.direction;
    diagnostics.moon_elevation_degrees =
        std::asin(math::clamp(state.moon.direction.y, -1.0F, 1.0F)) * math::kRadToDeg;
    diagnostics.moon_illuminated_fraction = state.moon.illuminated_fraction;
    diagnostics.star_visibility = state.star_visibility;
    diagnostics.time_domain = time_domain_name(time.domain);
    diagnostics.time_fraction = time.fraction;
    diagnostics.seconds_per_day = time.seconds_per_day;
    diagnostics.day_of_year = time.day_of_year;
    diagnostics.paused = time.paused;
    return diagnostics;
}

SkyPixelReport explain_sky_pixel(const SkyCompositionInputs& inputs, Vec3 direction,
                                 const CloudHistory* history) noexcept {
    SkyPixelReport report;
    report.direction = normalized_or(direction, Vec3{0.0F, 1.0F, 0.0F});

    const SkyCompositionSample sample = compose_sky(inputs, report.direction);
    report.radiance = sample.radiance;
    report.atmosphere_radiance = sample.atmosphere_radiance;
    report.stellar = sample.stellar;
    report.stars = sample.stars;
    report.aurora = sample.aurora;
    report.clouds = sample.clouds;
    report.cloud_transmittance = sample.cloud_transmittance;
    report.cloud_depth_metres = sample.cloud_depth_metres;
    report.dominant_layer = sample.dominant_cloud_layer;
    report.stats = sample.cloud_stats;

    if (inputs.tables != nullptr && inputs.tables->built()) {
        report.tables_sampled |= static_cast<u32>(SkyTableBit::Transmittance) |
                                 static_cast<u32>(SkyTableBit::MultipleScattering);
    }
    if (sample.from_table) {
        report.tables_sampled |= static_cast<u32>(SkyTableBit::SkyView);
    }

    // WHAT WAS AT THE POINT THAT DETERMINED THE PIXEL. The march already found it — the depth at
    // which the ray stopped being transparent — so the report asks the reconstruction about THAT
    // point rather than about an arbitrary one along the ray. A report that sampled the midpoint
    // would name a different cloud from the one the pixel shows.
    if (inputs.clouds != nullptr && inputs.clouds->map != nullptr &&
        sample.cloud_depth_metres > 0.0F && inputs.atmosphere != nullptr) {
        const Vec3 planet_sample =
            inputs.view.planet_relative + (report.direction * sample.cloud_depth_metres);
        const Vec3 world_sample =
            inputs.view.world_position + (report.direction * sample.cloud_depth_metres);
        const Vec3 position{world_sample.x, altitude_of(*inputs.atmosphere, planet_sample),
                            world_sample.z};

        const CloudDensitySample cloud = cloud_density(
            *inputs.clouds, position, inputs.time_seconds, inputs.cloud_quality.octaves);
        report.coverage = cloud.coverage;
        report.type = cloud.type;
        report.density = cloud.density;
        report.weather_version = cloud.version;
        if (cloud.dominant_layer < kMaxCloudLayers) {
            report.dominant_layer = cloud.dominant_layer;
        }

        // THE MAP CELL, AFTER ADVECTION. The cell a tuner has to edit is the one the wind has
        // carried over this pixel, not the one under it — and the difference is tens of cells after
        // an hour of play. Reporting the unadvected cell would send every tuner to the wrong place.
        const f32 cell = inputs.clouds->map->cell_metres();
        report.weather_cell_metres = cell;
        Vec3 wind{0.0F, 0.0F, 0.0F};
        if (report.dominant_layer < inputs.clouds->layers.count) {
            wind = inputs.clouds->layers.layers[report.dominant_layer].wind;
        }
        const auto drift_x = static_cast<f32>(static_cast<f64>(wind.x) * inputs.time_seconds);
        const auto drift_z = static_cast<f32>(static_cast<f64>(wind.z) * inputs.time_seconds);
        report.weather_cell_x =
            static_cast<i32>(std::floor((position.x - drift_x) / math::max(cell, 1.0F)));
        report.weather_cell_z =
            static_cast<i32>(std::floor((position.z - drift_z) / math::max(cell, 1.0F)));
    }

    if (inputs.clouds != nullptr && report.dominant_layer < inputs.clouds->layers.count) {
        const CloudLayer& layer = inputs.clouds->layers.layers[report.dominant_layer];
        report.dominant_layer_name = layer.name;
        report.dominant_layer_kind = cloud_layer_kind_name(layer.kind);
    }

    if (history != nullptr) {
        report.history = history->state;
        report.history_confidence = history->confidence;
        report.history_rejected_by_weather = history->rejected_by_weather;
    }
    return report;
}

CloudCostAttribution attribute_cloud_cost(const CloudMarchStats& stats, const CloudQuality& quality,
                                          u64 pixels) noexcept {
    CloudCostAttribution attribution;
    const u64 rays = pixels > 0 ? pixels : u64{1};
    attribution.view_samples = static_cast<u64>(stats.density_samples) * rays;
    attribution.light_samples = static_cast<u64>(stats.light_samples) * rays;

    const f64 total =
        static_cast<f64>(attribution.view_samples) + static_cast<f64>(attribution.light_samples);
    if (total > 0.0) {
        attribution.view_share =
            static_cast<f32>(static_cast<f64>(attribution.view_samples) / total);
        attribution.light_share =
            static_cast<f32>(static_cast<f64>(attribution.light_samples) / total);
    }

    // THE RESOLUTION LEVER'S PRICE. A march at half resolution covers a quarter of the pixels, so
    // the saving is `1 - scale^2` — and stating it as what the frame WOULD have cost is what lets a
    // tuner compare it against the step count's saving in the same unit.
    const f32 scale = math::clamp(quality.resolution_scale, 0.05F, 1.0F);
    const f64 full = total / static_cast<f64>(scale * scale);
    attribution.samples_at_full_resolution = static_cast<u64>(full);
    attribution.resolution_saving = 1.0F - (scale * scale);

    u32 attributed = 0;
    for (const u32 steps_here : stats.steps_in_layer) {
        attributed += steps_here;
    }
    if (attributed > 0) {
        for (u32 index = 0; index < kMaxCloudLayers; ++index) {
            attribution.layer_share[index] =
                static_cast<f32>(stats.steps_in_layer[index]) / static_cast<f32>(attributed);
        }
    }
    attribution.layers_touched = stats.layers_touched;
    if (stats.steps > 0) {
        attribution.empty_fraction =
            static_cast<f32>(stats.empty_steps) / static_cast<f32>(stats.steps);
    }
    return attribution;
}

Vec3 visualise(SkyDebugView view, const SkyPixelReport& report) noexcept {
    switch (view) {
        case SkyDebugView::CloudCoverage:
            return ramp(report.coverage);
        case SkyDebugView::CloudType:
            // Stratus blue, cumulonimbus red: the two ends of the type axis, so that a storm front
            // is a colour boundary rather than a gradient a reader has to measure.
            return lerp(Vec3{0.25F, 0.45F, 0.95F}, Vec3{0.95F, 0.25F, 0.15F},
                        math::saturate(report.type));
        case SkyDebugView::RayMarchSteps:
            // Normalised against the longest march this module will run — `CloudQuality`'s
            // cinematic step count — so that two tiers' step views are comparable.
            return ramp(static_cast<f32>(report.stats.steps) / 192.0F);
        case SkyDebugView::HistoryConfidence:
            // A weather rejection is drawn MAGENTA rather than as zero confidence: the two have
            // different causes and a tuner reading a black screen cannot tell which it is.
            return report.history_rejected_by_weather ? Vec3{1.0F, 0.0F, 1.0F}
                                                      : ramp(report.history_confidence);
        case SkyDebugView::CloudDensitySlice:
            return ramp(math::saturate(report.density * 8.0F));
        case SkyDebugView::CloudShadowField:
            return visualise_scalar(view, report.cloud_transmittance);
        case SkyDebugView::AtmosphereTables:
            // Which tables this pixel read, as three channels: transmittance, multiple scattering,
            // sky view. A pixel that read none is black, which is the case worth finding.
            return Vec3{
                has_table(report.tables_sampled, SkyTableBit::Transmittance) ? 1.0F : 0.0F,
                has_table(report.tables_sampled, SkyTableBit::MultipleScattering) ? 1.0F : 0.0F,
                has_table(report.tables_sampled, SkyTableBit::SkyView) ? 1.0F : 0.0F};
        case SkyDebugView::CelestialState:
            // The direction itself, mapped into the unit cube. The one view where the value IS a
            // direction, and a ramp would throw two thirds of it away.
            return (report.direction * 0.5F) + Vec3{0.5F, 0.5F, 0.5F};
        case SkyDebugView::None:
        case SkyDebugView::Count:
            break;
    }
    return Vec3{0.0F, 0.0F, 0.0F};
}

Vec3 visualise_scalar(SkyDebugView view, f32 value) noexcept {
    switch (view) {
        case SkyDebugView::CloudShadowField:
            // A shadow field is a TRANSMITTANCE, so it is drawn as light rather than through a
            // ramp: full sun is white and a storm's shadow is black, which is what the value means.
            return Vec3{math::saturate(value), math::saturate(value), math::saturate(value)};
        case SkyDebugView::CloudCoverage:
        case SkyDebugView::CloudType:
        case SkyDebugView::CloudDensitySlice:
        case SkyDebugView::RayMarchSteps:
        case SkyDebugView::HistoryConfidence:
            return ramp(value);
        case SkyDebugView::AtmosphereTables:
        case SkyDebugView::CelestialState:
        case SkyDebugView::None:
        case SkyDebugView::Count:
            break;
    }
    return Vec3{0.0F, 0.0F, 0.0F};
}

}  // namespace cy::rendering::sky
