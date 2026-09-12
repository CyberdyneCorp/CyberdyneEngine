#pragma once
// WHAT DETERMINED THIS PIXEL, AND WHERE THE COST WENT. M10 task 3.3.
//
// `atmosphere-sky-and-clouds` — "Atmosphere and cloud diagnostics": "The engine SHALL provide:
// visualisation of the cloud coverage field and cloud type, ray march step counts, temporal history
// confidence, the cloud shadow field, density slices, the atmospheric tables and their update
// frequency, and the celestial model's state. FOR ANY PIXEL OF SKY OR CLOUD, the tooling SHALL be
// able to report what determined it: coverage, type, layer, the weather source, and the tables
// sampled. Cloud rendering cost SHALL be attributable to steps, resolution, and layers, since cloud
// rendering is notoriously difficult to tune without that."
//
// ================================================================================================
// WHY THIS IS A HEADER AND NOT A DEBUG DRAW CALL
// ================================================================================================
//
// The requirement asks for a REPORT, not a picture: "the tooling SHALL be able to report what
// determined it". A `debug_draw_clouds()` that painted coverage into a render target would satisfy
// the visualisation half and none of the attribution half, and it would be unavailable to a test,
// to the editor's inspector, and to a bug report pasted into a ticket.
//
// So every entry point here returns a VALUE. `visualise()` turns one of those values into the
// colour a debug view draws, which is the visualisation half expressed as a function of the report
// rather than as a second path that could disagree with it.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/rendering/sky/celestial.h>
#include <cy/rendering/sky/clouds.h>
#include <cy/rendering/sky/composition.h>
#include <cy/rendering/sky/tables.h>

namespace cy::rendering::sky {

/// The debug views the requirement lists, one enumerator each. An enumeration rather than a set of
/// booleans because exactly one is drawn at a time and two at once is a picture of neither.
enum class SkyDebugView : u8 {
    None = 0,
    CloudCoverage,
    CloudType,
    RayMarchSteps,
    HistoryConfidence,
    CloudShadowField,
    CloudDensitySlice,
    AtmosphereTables,
    CelestialState,
    Count,
};

[[nodiscard]] const char* sky_debug_view_name(SkyDebugView view) noexcept;

/// Which tables a sample read. A bitmask so that a report can say "this pixel read the sky view and
/// nothing else", which is the difference between a table that is working and a table that is
/// built and bypassed.
enum class SkyTableBit : u32 {
    None = 0,
    Transmittance = 1U << 0U,
    MultipleScattering = 1U << 1U,
    SkyView = 1U << 2U,
    AerialPerspective = 1U << 3U,
};

[[nodiscard]] constexpr u32 operator|(SkyTableBit a, SkyTableBit b) noexcept {
    return static_cast<u32>(a) | static_cast<u32>(b);
}

[[nodiscard]] constexpr bool has_table(u32 mask, SkyTableBit bit) noexcept {
    return (mask & static_cast<u32>(bit)) != 0U;
}

// ================================================================================================
// THE TABLES AND THEIR UPDATE FREQUENCY
// ================================================================================================

/// "the atmospheric tables and their update frequency". Frequency and not a count: a table rebuilt
/// twice in a session and a table rebuilt twice a frame have the same count.
struct TableDiagnostics {
    u32 transmittance_rebuilds = 0;
    u64 transmittance_directions = 0;
    u32 multiple_scattering_rebuilds = 0;
    u64 multiple_scattering_directions = 0;

    u32 sky_view_full_rebuilds = 0;
    u32 sky_view_incremental_updates = 0;
    u64 sky_view_rows_rebuilt = 0;
    u64 sky_view_directions = 0;
    /// The worst per-row staleness the incremental scheme has let build up, in radians of sun
    /// movement. The bound the row budget buys, reported rather than assumed.
    f32 worst_row_staleness = 0.0F;

    /// Per second, over the window the caller states.
    f32 sky_view_updates_per_second = 0.0F;
    f32 sky_view_rows_per_second = 0.0F;
    /// Rows re-integrated per update, as a fraction of the table's rows. The number that says
    /// whether "incremental" is true: a full rebuild is 1.
    f32 mean_rebuild_fraction = 0.0F;
};

[[nodiscard]] TableDiagnostics table_diagnostics(const AtmosphereTables& tables,
                                                 const IncrementalSkyView& sky_view,
                                                 f32 seconds_observed) noexcept;

// ================================================================================================
// THE CELESTIAL MODEL'S STATE
// ================================================================================================

struct CelestialDiagnostics {
    Vec3 sun_direction{0.0F, 1.0F, 0.0F};
    f32 sun_elevation_degrees = 0.0F;
    f32 sun_azimuth_degrees = 0.0F;
    Vec3 moon_direction{0.0F, 1.0F, 0.0F};
    f32 moon_elevation_degrees = 0.0F;
    f32 moon_illuminated_fraction = 0.0F;
    f32 star_visibility = 0.0F;
    /// The clock, so that "the sky is stuck" can be attributed to a paused time of day rather than
    /// to the celestial model.
    const char* time_domain = "";
    f32 time_fraction = 0.0F;
    f32 seconds_per_day = 0.0F;
    f32 day_of_year = 0.0F;
    bool paused = false;
};

[[nodiscard]] CelestialDiagnostics celestial_diagnostics(const TimeOfDay& time,
                                                         const CelestialState& state) noexcept;

// ================================================================================================
// WHAT DETERMINED THIS PIXEL
// ================================================================================================

/// The answer to "why is this pixel this colour", in the order the requirement lists the question:
/// coverage, type, layer, the weather source, and the tables sampled.
struct SkyPixelReport {
    Vec3 direction{0.0F, 1.0F, 0.0F};
    Vec3 radiance{0.0F, 0.0F, 0.0F};

    /// The elements, separately, so a pixel that is too bright can be attributed to one of them.
    Vec3 atmosphere_radiance{0.0F, 0.0F, 0.0F};
    Vec3 stellar{0.0F, 0.0F, 0.0F};
    Vec3 stars{0.0F, 0.0F, 0.0F};
    Vec3 aurora{0.0F, 0.0F, 0.0F};
    Vec3 clouds{0.0F, 0.0F, 0.0F};

    // --- coverage, type, layer ---
    f32 coverage = 0.0F;
    f32 type = 0.0F;
    f32 density = 0.0F;
    f32 cloud_transmittance = 1.0F;
    f32 cloud_depth_metres = -1.0F;
    u32 dominant_layer = kMaxCloudLayers;
    /// The layer's authored name and its kind. A number is not an answer to "which layer".
    const char* dominant_layer_name = "";
    const char* dominant_layer_kind = "none";

    // --- the weather source ---
    /// The map cell the dominant sample read, AFTER advection — which is the cell a tuner has to
    /// edit to change this pixel, and is not the cell under the pixel.
    i32 weather_cell_x = 0;
    i32 weather_cell_z = 0;
    f32 weather_cell_metres = 0.0F;
    u32 weather_version = 0;

    // --- the tables sampled ---
    u32 tables_sampled = 0;

    // --- cost and history ---
    CloudMarchStats stats;
    HistoryState history = HistoryState::Unrepresentable;
    f32 history_confidence = 0.0F;
    bool history_rejected_by_weather = false;
};

/// Explain one direction of sky. `history` is optional: a still frame has none, and a report that
/// required one could not be produced from a screenshot.
[[nodiscard]] SkyPixelReport explain_sky_pixel(const SkyCompositionInputs& inputs, Vec3 direction,
                                               const CloudHistory* history) noexcept;

// ================================================================================================
// COST ATTRIBUTION
// ================================================================================================

/// "Cloud rendering cost SHALL be attributable to steps, resolution, and layers."
///
/// Three shares that sum to one, plus the absolute counts they are derived from. Shares rather than
/// milliseconds because this module has no device and a millisecond that was not measured is worse
/// than no millisecond at all; the arbiter supplies the scale (`arbiter/subsystem.h` says so in as
/// many words) and this supplies the proportions.
struct CloudCostAttribution {
    /// Density samples along the view rays, and along the rays to the sun. The two halves of the
    /// march, and the reason `light_steps` is a separate lever from `steps`.
    u64 view_samples = 0;
    u64 light_samples = 0;
    /// What the same frame would have cost at full resolution. The resolution lever's price.
    u64 samples_at_full_resolution = 0;

    f32 view_share = 0.0F;
    f32 light_share = 0.0F;
    /// The fraction saved by marching at reduced resolution, in [0, 1).
    f32 resolution_saving = 0.0F;

    /// Each layer's share of the steps that found something. A layer at 60% is the layer to
    /// simplify, and without this number a tuner halves the step count instead.
    f32 layer_share[kMaxCloudLayers] = {};
    u32 layers_touched = 0;
    /// Steps that found nothing, as a fraction. High means an empty-space skip would pay; this is
    /// the number that says whether to build one.
    f32 empty_fraction = 0.0F;
};

/// Attribute one frame's cost. `pixels` is how many rays were marched — the reduced-resolution
/// count, not the display's.
[[nodiscard]] CloudCostAttribution attribute_cloud_cost(const CloudMarchStats& stats,
                                                        const CloudQuality& quality,
                                                        u64 pixels) noexcept;

// ================================================================================================
// VISUALISATION
// ================================================================================================

/// The colour a debug view draws for one pixel, in [0, 1] per channel.
///
/// A function OF THE REPORT, so a debug view cannot show something the report does not say. That is
/// the whole reason it takes a `SkyPixelReport` rather than the inputs: two paths to one picture is
/// how a visualisation ends up lying about the thing it visualises.
[[nodiscard]] Vec3 visualise(SkyDebugView view, const SkyPixelReport& report) noexcept;

/// The same for the views that are fields rather than pixels: the cloud shadow field and a density
/// slice are sampled at a world position, not along a ray.
[[nodiscard]] Vec3 visualise_scalar(SkyDebugView view, f32 value) noexcept;

}  // namespace cy::rendering::sky
