// The environment inspector: the composition, explained by the functions that produced it.
// M10 tasks 3.1 and 3.2.

#include <cy/weather/diagnostics.h>

#include <cy/weather/system.h>

#include <cmath>

namespace cy::weather {

const char* rain_source_kind_name(RainSourceKind kind) noexcept {
    switch (kind) {
        case RainSourceKind::Cell:
            return "cell";
        case RainSourceKind::Storm:
            return "storm";
        case RainSourceKind::OrographicGain:
            return "orographic-gain";
        case RainSourceKind::RainShadow:
            return "rain-shadow";
        default:
            return "unknown";
    }
}

namespace {

/// The rain, decomposed. Every term comes from the SAME function the step called — the storms from
/// `storm_contribution_at()`, the uplift from `WeatherCells::uplift_at()` — so an explanation can
/// name something the simulation did not do only if the simulation and the inspector disagree about
/// which function to call, which is a compile error rather than a wrong number.
void explain_rain(const WeatherSystem& system, const world::WorldVec3d& at,
                  const EnvironmentSample& sample, PrecipitationExplanation& out) noexcept {
    out.type = sample.precipitation_type;
    out.total_mm_per_hour = sample.precipitation_mm_per_hour;

    StormId ids[kMaxInspectedSources];
    StormContribution contributions[kMaxInspectedSources];
    const usize storms = system.storms().contributions_at(
        at.x, at.z, Span<StormId>(ids, kMaxInspectedSources),
        Span<StormContribution>(contributions, kMaxInspectedSources));

    f32 from_storms = 0.0F;
    for (usize index = 0; index < storms && out.count < kMaxInspectedSources; ++index) {
        if (contributions[index].precipitation_mm_per_hour <= 0.0F) {
            continue;
        }
        from_storms += contributions[index].precipitation_mm_per_hour;
        out.sources[out.count] = RainContribution{
            RainSourceKind::Storm, contributions[index].precipitation_mm_per_hour, ids[index]};
        ++out.count;
    }

    const WeatherCellSample cell = system.cells().sample(at, WeatherScale::Local);
    out.uplift_mps = system.cells().uplift_at(at.x, at.z, cell.state.wind);

    // The cell's own share is what is left once the storms are accounted for. It is computed as a
    // REMAINDER rather than sampled separately, because the cell state already contains the storms
    // that were applied during the step — and reporting both in full would double-count them.
    const f32 from_cell = out.total_mm_per_hour - from_storms;
    if (out.count < kMaxInspectedSources) {
        out.sources[out.count] = RainContribution{RainSourceKind::Cell, from_cell, StormId{}};
        ++out.count;
    }

    if (out.count < kMaxInspectedSources && out.uplift_mps != 0.0F) {
        // Positive uplift is a GAIN and negative uplift is a SHADOW, and the shadow is reported
        // negative. A "shadow" reported as a positive number would invert the sign a designer
        // reads, which is the one thing an explanation must not do.
        const bool windward = out.uplift_mps > 0.0F;
        const f32 magnitude = windward ? out.uplift_mps * cell.state.humidity * 3.0F
                                       : -out.total_mm_per_hour *
                                             (1.0F - (1.0F / (1.0F + (0.25F * -out.uplift_mps))));
        out.sources[out.count] =
            RainContribution{windward ? RainSourceKind::OrographicGain : RainSourceKind::RainShadow,
                             magnitude, StormId{}};
        ++out.count;
    }
}

}  // namespace

EnvironmentInspection EnvironmentInspector::inspect(const world::WorldVec3d& at) const noexcept {
    EnvironmentInspection out;
    if (!bound()) {
        return out;
    }
    out.position = at;
    // The inspector is a PRESENTATION reader: it is a developer tool and it is allowed to see the
    // turbulence residual, which is most of what it exists to visualise. That is the firewall used
    // rather than bypassed — a tool that wanted the authoritative half asks for it by class.
    out.sample = system_->sample(at, SampleQuality::HighFrequency,
                                 determinism::SimulationClass::Presentation);

    const WeatherCellSample cell = system_->cells().sample(at, WeatherScale::Local);
    out.cell_x = system_->cells().cell_x_of(at.x);
    out.cell_z = system_->cells().cell_z_of(at.z);
    out.scale = cell.scale;
    out.cell_metres = cell.cell_metres;
    out.climate = system_->cells().climate_at(at.x, at.z);
    out.potential = climate_biome_potential(out.climate);

    if (system_->occlusion() != nullptr) {
        out.shelter = system_->occlusion()->shelter_at(at);
    }

    if (const environment::FieldStore* store = system_->fields().store(); store != nullptr) {
        if (system_->fields().declared(WeatherField::Wetness)) {
            out.wetness =
                store->sample_deterministic(system_->fields().id(WeatherField::Wetness), at)
                    .value.x();
        }
        if (system_->fields().declared(WeatherField::SnowDepth)) {
            out.snow_depth_metres =
                store->sample_deterministic(system_->fields().id(WeatherField::SnowDepth), at)
                    .value.x();
        }
    }

    // ONE WALK produces both the sample and the breakdown — see wind.h's `Emitter`, and
    // diagnostics.h's header note. The sum below is therefore the sampled wind exactly, which is
    // what `test_diagnostics.cpp` measures.
    out.wind.count = static_cast<u32>(system_->wind().explain(
        at, determinism::SimulationClass::Presentation, system_->moment(), system_->seconds(),
        Span<WindContribution>(out.wind.sources, kMaxInspectedSources)));
    out.wind.sample = out.sample.wind;
    for (u32 index = 0; index < out.wind.count; ++index) {
        out.wind.total.x += out.wind.sources[index].velocity.x;
        out.wind.total.y += out.wind.sources[index].velocity.y;
        out.wind.total.z += out.wind.sources[index].velocity.z;
    }

    explain_rain(*system_, at, out.sample, out.precipitation);
    return out;
}

Status EnvironmentInspector::storm_path(StormId id, u32 samples, f32 interval_seconds,
                                        Array<StormPathPoint>& out) const noexcept {
    if (!bound()) {
        return fail(ErrorCode::Unavailable, "weather: the inspector is not bound to a system");
    }
    const Storm* storm = system_->storms().find(id);
    if (storm == nullptr) {
        return fail(ErrorCode::NotFound, "weather: no storm with that identity");
    }
    // RECONSTRUCTED, NOT RECORDED, and the header says so. The registry keeps no history — a storm
    // is thirty-seven bytes and a track would be a log — so the path is extrapolated backward along
    // the storm's own velocity, which is exact for a storm that has not been scripted to turn.
    for (u32 index = 0; index < samples; ++index) {
        const f32 back = static_cast<f32>(index) * interval_seconds;
        if (back > storm->age_seconds) {
            break;
        }
        StormPathPoint point;
        point.position.x = storm->position.x - static_cast<f64>(storm->velocity.x * back);
        point.position.y = storm->position.y;
        point.position.z = storm->position.z - static_cast<f64>(storm->velocity.y * back);
        point.radius_metres = storm->radius_metres;
        point.intensity = storm->intensity;
        point.tick = system_->tick();
        if (Status pushed = out.push_back(point); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status EnvironmentInspector::cell_overlay(CellOverlayQuantity quantity, f64 min_x, f64 min_z,
                                          f64 max_x, f64 max_z,
                                          Array<CellOverlayQuad>& out) const noexcept {
    if (!bound()) {
        return fail(ErrorCode::Unavailable, "weather: the inspector is not bound to a system");
    }
    const WeatherCells& cells = system_->cells();
    const WeatherGridConfig& config = cells.config();
    const auto cell_metres = static_cast<f64>(config.regional_cell_metres);
    const f64 half = cell_metres * 0.5;
    for (i32 k = cells.cell_z_of(min_z); k <= cells.cell_z_of(max_z); ++k) {
        for (i32 i = cells.cell_x_of(min_x); i <= cells.cell_x_of(max_x); ++i) {
            const WeatherState* cell = cells.regional_cell(i, k);
            if (cell == nullptr) {
                continue;
            }
            const f64 x = config.origin_x + (static_cast<f64>(i) * cell_metres);
            const f64 z = config.origin_z + (static_cast<f64>(k) * cell_metres);
            CellOverlayQuad quad;
            quad.min_x = x - half;
            quad.min_z = z - half;
            quad.max_x = x + half;
            quad.max_z = z + half;
            quad.scale = WeatherScale::Regional;
            switch (quantity) {
                case CellOverlayQuantity::Temperature:
                    quad.value = cell->temperature_celsius;
                    break;
                case CellOverlayQuantity::Humidity:
                    quad.value = cell->humidity;
                    break;
                case CellOverlayQuantity::Pressure:
                    quad.value = cell->pressure_hpa;
                    break;
                case CellOverlayQuantity::WindSpeed:
                    quad.value =
                        std::sqrt((cell->wind.x * cell->wind.x) + (cell->wind.y * cell->wind.y));
                    break;
                case CellOverlayQuantity::CloudCoverage:
                    quad.value = cell->cloud_coverage;
                    break;
                case CellOverlayQuantity::Precipitation:
                    quad.value = cell->precipitation_mm_per_hour;
                    break;
                case CellOverlayQuantity::Visibility:
                    quad.value = cell->visibility_metres;
                    break;
            }
            if (Status pushed = out.push_back(quad); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

}  // namespace cy::weather
