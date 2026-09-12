// The environment sample: one answer at a declared quality. M10 task 3.1.

#include <cy/weather/sample.h>

#include <algorithm>
#include <cmath>

namespace cy::weather {

namespace {

[[nodiscard]] f32 clamp01(f32 value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

}  // namespace

const char* sample_quality_name(SampleQuality quality) noexcept {
    switch (quality) {
        case SampleQuality::Macro:
            return "macro";
        case SampleQuality::Gameplay:
            return "gameplay";
        case SampleQuality::HighFrequency:
            return "high-frequency";
    }
    return "unknown";
}

f32 wetness_potential_of(const WeatherState& state) noexcept {
    // The equilibrium a surface reaches under these conditions. Rain saturates quickly — a few
    // millimetres an hour is a wet road — and humidity alone dampens without wetting, which is the
    // difference between a misty morning and a downpour.
    const f32 from_rain = clamp01(state.precipitation_mm_per_hour * 0.6F);
    const f32 from_humidity = clamp01((state.humidity - 0.7F) * 1.4F);
    const f32 wet = (from_rain > from_humidity) ? from_rain : from_humidity;
    // Below freezing a surface does not wet, it ices: what falls accumulates as snow instead, which
    // is `accumulation.h`'s other field.
    return (state.temperature_celsius <= 0.0F) ? wet * 0.15F : wet;
}

EnvironmentSample EnvironmentSampler::sample(const world::WorldVec3d& at, SampleQuality quality,
                                             determinism::SimulationClass reader) const noexcept {
    EnvironmentSample out;
    if (!bound()) {
        return out;
    }

    // A HighFrequency request from an authoritative reader is DOWNGRADED rather than refused, and
    // `quality` says so. See sample.h's header note.
    SampleQuality effective = quality;
    if (effective == SampleQuality::HighFrequency &&
        !determinism::may_read(reader, determinism::SimulationClass::Presentation)) {
        effective = SampleQuality::Gameplay;
    }

    const WeatherScale finest =
        (effective == SampleQuality::HighFrequency) ? WeatherScale::Local : WeatherScale::Regional;
    const WeatherCellSample cell = cells_->sample(at, finest);
    out.temperature_celsius = cell.state.temperature_celsius;
    out.humidity = cell.state.humidity;
    out.pressure_hpa = cell.state.pressure_hpa;
    out.precipitation_mm_per_hour = cell.state.precipitation_mm_per_hour;
    out.precipitation_type = cell.state.precipitation_type;
    out.cloud_coverage = cell.state.cloud_coverage;
    out.visibility_metres = cell.state.visibility_metres;
    out.wetness_potential = wetness_potential_of(cell.state);
    out.freezing = cell.state.temperature_celsius <= 0.0F;
    out.quality = effective;
    out.scale = cell.scale;
    out.cell_metres = cell.cell_metres;
    out.inside_grid = cell.inside_grid;

    if (effective == SampleQuality::Macro) {
        // MACRO: the cell's own wind and nothing composed on top. An agent evaluating a hundred
        // positions pays one grid lookup each, which is the whole point of the quality declaration.
        // The value is the same regional wind the finer qualities start from, so a macro answer and
        // a gameplay answer cannot contradict each other — see sample.h.
        out.wind.base = Vec3{cell.state.wind.x, cell.state.vertical_wind, cell.state.wind.y};
        return out;
    }

    out.wind = wind_->sample(at, reader, when_, seconds_);
    return out;
}

Status EnvironmentSampler::sample_many(Span<const world::WorldVec3d> positions,
                                       SampleQuality quality, determinism::SimulationClass reader,
                                       Span<EnvironmentSample> out) const noexcept {
    if (out.size() < positions.size()) {
        return fail(ErrorCode::BufferTooSmall,
                    "weather: the environment batch output is shorter than its input");
    }
    // Nothing is allocated here and nothing is allocated below: the batch is a loop over the
    // caller's span. "Sampling SHALL be batchable and SHALL NOT allocate."
    for (usize index = 0; index < positions.size(); ++index) {
        out[index] = sample(positions[index], quality, reader);
    }
    return ok();
}

}  // namespace cy::weather
