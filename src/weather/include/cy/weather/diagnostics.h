#pragma once
// THE ENVIRONMENT INSPECTOR: not one number, but which storm, which slope and which volume made it.
// M10 tasks 3.1 and 3.2.
//
// `weather-and-wind` — "Environment diagnostics": the engine "SHALL provide an environment
// inspector reporting, for a selected position: temperature, humidity, pressure, wind WITH ITS
// CONTRIBUTING SOURCES, precipitation rate and type, cloud coverage, visibility, wetness, snow
// depth, climate region, and the weather sources that produced them"; "It SHALL EXPLAIN
// COMPOSITION: a rain rate reported as a storm's contribution, a terrain rain shadow reduction, and
// a local volume's addition, RATHER THAN AS ONE NUMBER"; "Wind SHALL be visualisable as vectors
// with per-source contribution, and weather cells, storm paths, and field values SHALL be
// visualisable over the world."
//
// ================================================================================================
// THE EXPLANATION IS THE COMPOSITION, NOT A SECOND ONE
// ================================================================================================
//
// `WindComposer::explain()` and `WindComposer::sample()` are two consumers of ONE walk — see
// wind.h's `Emitter`. The inspector calls `explain()`, so the sum of the reported contributions IS
// the sampled wind, exactly, and `test_diagnostics.cpp` requires that equality bit for bit. An
// inspector that re-derived the terms would be an inspector that can explain a wind the simulation
// did not produce, which is worse than no inspector: it is a plausible wrong answer.
//
// The same holds for rain. `PrecipitationExplanation` carries the base rate the cell holds, each
// storm's own contribution through `storm_contribution_at()`, and the orographic terms through
// `WeatherCells::uplift_at()` — the same functions the step calls.
//
// ================================================================================================
// WHAT THIS FILE DOES NOT DO
// ================================================================================================
//
// It draws nothing. `StormPath` is a list of positions and `CellOverlay` is a list of rectangles
// with values; turning either into geometry is the editor's, which is why this header names no
// renderer and this module links none. The specification asks for things to be VISUALISABLE, and
// what makes them visualisable is that the data can be got out.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/weather/accumulation.h>
#include <cy/weather/climate.h>
#include <cy/weather/precipitation.h>
#include <cy/weather/sample.h>
#include <cy/weather/storm.h>
#include <cy/weather/wind.h>

namespace cy::weather {

class WeatherSystem;

/// The most sources one inspection reports separately. Beyond it the tail is summed into an `Other`
/// row, because an inspector that allocated per query would be one a profiler view cannot run.
inline constexpr u32 kMaxInspectedSources = 16;

/// Where a share of the rain came from.
enum class RainSourceKind : u8 {
    /// The weather cell's own state: the prevailing, advected rate.
    Cell = 0,
    Storm,
    /// The windward enhancement: air forced up a slope rains.
    OrographicGain,
    /// The lee reduction. Negative by construction — a rain shadow REMOVES rain, and reporting it
    /// as a positive "shadow" would invert the sign a designer reads.
    RainShadow,
    kCount,
};

[[nodiscard]] const char* rain_source_kind_name(RainSourceKind kind) noexcept;

struct RainContribution {
    RainSourceKind kind = RainSourceKind::Cell;
    /// Millimetres per hour this source contributes. Signed; see `RainShadow`.
    f32 mm_per_hour = 0.0F;
    /// The storm, where there is one.
    StormId storm;
};

/// "Why is it raining here", answered as a composition.
struct PrecipitationExplanation {
    RainContribution sources[kMaxInspectedSources];
    u32 count = 0;
    /// The sum. Equal to the sampled rate, which is the property that makes this an explanation
    /// rather than an opinion.
    f32 total_mm_per_hour = 0.0F;
    PrecipitationType type = PrecipitationType::None;
    /// The uplift the orographic model found, m/s. Positive is windward, negative is lee.
    f32 uplift_mps = 0.0F;
};

/// "Why is the wind doing that", answered as a composition.
struct WindExplanation {
    WindContribution sources[kMaxInspectedSources];
    u32 count = 0;
    WindSample sample;
    /// The sum of the reported contributions. Equal to `sample.full()` — the equality the suite
    /// checks — and carried separately so that a failure reports both numbers rather than a bool.
    Vec3 total;
};

/// Everything the inspector reports for a position. The specification's list, member for member,
/// plus the two explanations.
struct EnvironmentInspection {
    world::WorldVec3d position;
    EnvironmentSample sample;

    /// The accumulated fields, read back through the store. Zero with no store bound.
    f32 wetness = 0.0F;
    f32 snow_depth_metres = 0.0F;

    /// The climate the position sits in, and the biome it tends toward.
    ClimateSample climate;
    BiomePotential potential;
    /// Which weather cell answered, in grid coordinates, and at which scale.
    i32 cell_x = 0;
    i32 cell_z = 0;
    WeatherScale scale = WeatherScale::Regional;
    f32 cell_metres = 0.0F;

    ShelterSample shelter;
    WindExplanation wind;
    PrecipitationExplanation precipitation;
};

/// A storm's track, for a world-space overlay.
struct StormPathPoint {
    world::WorldVec3d position;
    f32 radius_metres = 0.0F;
    f32 intensity = 0.0F;
    u64 tick = 0;
};

/// One rectangle of the weather grid, with the value being visualised. What an overlay draws.
struct CellOverlayQuad {
    f64 min_x = 0.0;
    f64 min_z = 0.0;
    f64 max_x = 0.0;
    f64 max_z = 0.0;
    f32 value = 0.0F;
    WeatherScale scale = WeatherScale::Regional;
};

/// Which quantity a cell overlay carries.
enum class CellOverlayQuantity : u8 {
    Temperature = 0,
    Humidity,
    Pressure,
    WindSpeed,
    CloudCoverage,
    Precipitation,
    Visibility,
};

/// The inspector. It holds a system by reference and answers questions about it; it owns nothing
/// and changes nothing, so leaving one alive costs a pointer.
class EnvironmentInspector {
public:
    EnvironmentInspector() = default;
    explicit EnvironmentInspector(const WeatherSystem& system) noexcept : system_(&system) {}

    [[nodiscard]] EnvironmentInspection inspect(const world::WorldVec3d& at) const noexcept;

    /// Sample a storm's track over the past `samples` recorded positions. The registry keeps no
    /// history, so this extrapolates BACKWARD from the storm's own velocity and age and says so —
    /// an honest reconstruction, not a recording nobody made.
    [[nodiscard]] Status storm_path(StormId id, u32 samples, f32 interval_seconds,
                                    Array<StormPathPoint>& out) const noexcept;

    /// Every regional cell overlapping a rectangle, carrying one quantity.
    [[nodiscard]] Status cell_overlay(CellOverlayQuantity quantity, f64 min_x, f64 min_z, f64 max_x,
                                      f64 max_z, Array<CellOverlayQuad>& out) const noexcept;

    [[nodiscard]] bool bound() const noexcept { return system_ != nullptr; }

private:
    const WeatherSystem* system_ = nullptr;
};

}  // namespace cy::weather
