#pragma once
// WETNESS AND SNOW: what precipitation leaves behind, and what takes it away again. M10 task 3.2.
//
// `weather-and-wind` — "Wetness and snow accumulation": precipitation "SHALL accumulate into
// WETNESS and SNOW DEPTH fields, and both SHALL DECAY: wetness by evaporation driven by
// temperature, sun exposure, and wind; snow by melting driven by temperature and sun"; "Materials
// SHALL sample wetness and snow rather than being told about them, and visual accumulation SHALL be
// composited through the runtime virtual texture path (see `virtual-texturing`) rather than by
// modifying geometry"; "Accumulation SHALL be PERSISTABLE where a project requires it: a region
// that was snowed on and unloaded SHALL retain its snow state"; "Foliage, terrain, water,
// navigation, and gameplay SHALL be able to consume these fields."
//
// ================================================================================================
// SNOW IS NOT GEOMETRY, AND THAT IS A PROPERTY OF THIS FILE'S OUTPUT TYPE
// ================================================================================================
//
// The only thing this file writes is `environment::FieldValue` through a `FieldWriter`. There is no
// mesh here, no displacement, no height, and no terrain handle — src/terrain/ is not on this
// module's link line at all. A consumer that wants snow on a surface samples `snow-depth` and
// composites it, which `terrain`'s own material stack does through the runtime virtual texture
// producer. "Snow is a field composited into surfaces, not a modification of terrain geometry" is
// therefore unspellable here rather than merely unwritten.
//
// ================================================================================================
// PERSISTENCE IS A DECLARATION, AND IT IS THE SUBSTRATE'S
// ================================================================================================
//
// Both fields are declared `persistent` and classified `Persistent`, which is what puts them in the
// world persistence overlay through `environment-fields` rather than through a save path of this
// module's own. A region that was snowed on and unloaded keeps its snow because its tile is
// persistent state, not because anything here remembered it.
//
// ================================================================================================
// THE TARGET IS THE SAMPLE'S OWN DERIVED VALUE
// ================================================================================================
//
// Wetness heads for `wetness_potential_of(state)` — literally the function `EnvironmentSample`
// reports as `wetness_potential`. Two models for "how wet does this get" would drift, and the one
// that drifts is always the one nobody is looking at.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/environment/store.h>
#include <cy/weather/fields.h>
#include <cy/weather/precipitation.h>

namespace cy::weather {

class EnvironmentSampler;

/// The rates. Declared rather than buried, because a designer tuning how long a road stays wet
/// after a shower is tuning exactly these numbers.
struct AccumulationModel {
    /// Fraction of the gap to saturation closed per second at one millimetre per hour of rain.
    f32 wetting_per_mm_hour = 0.0025F;
    /// Base evaporation, fraction of the current wetness removed per second at 20 C, no sun, no
    /// wind.
    f32 evaporation_base = 0.00012F;
    /// Multiplier on evaporation per degree Celsius above zero.
    f32 evaporation_per_celsius = 0.045F;
    /// Multiplier on evaporation at full sun exposure, over and above the base.
    f32 evaporation_sun_gain = 1.4F;
    /// Multiplier on evaporation per metre per second of wind.
    f32 evaporation_wind_gain = 0.09F;

    /// Metres of snow per millimetre per hour of frozen precipitation per second. Ten to one by
    /// volume is the usual fresh-snow ratio, and this is that ratio in the units the fields carry.
    f32 snow_per_mm_hour = 2.8e-6F;
    /// Metres of snow melted per second per degree Celsius above the melting point.
    f32 melt_per_celsius = 1.1e-6F;
    /// Multiplier on melt at full sun exposure.
    f32 melt_sun_gain = 1.6F;
    /// Degrees Celsius at which snow melts. A dial, because a project's "snow" may be ash.
    f32 melting_point_celsius = 0.4F;
};

/// How much sun reaches the ground here, [0, 1]. Cloud cover and shelter, composed — the two things
/// that decide whether a puddle in a courtyard dries.
[[nodiscard]] f32 sun_exposure_of(const WeatherState& state, const ShelterSample& shelter) noexcept;

/// One step of the wetness and snow model at one point. Free and pure, so the field publication,
/// the editor's fast-forward and a test all run the same arithmetic.
///
/// `seconds` may be a frame or a day: the wetting and the decay are both exponential in the gap, so
/// two half steps and one whole step leave the same value — which is what makes `advance_days()`
/// the runtime's own model rather than a preview of it.
struct AccumulationPoint {
    f32 wetness = 0.0F;
    f32 snow_depth_metres = 0.0F;
};

[[nodiscard]] AccumulationPoint step_accumulation(const AccumulationModel& model,
                                                  const AccumulationPoint& current,
                                                  const WeatherState& state, f32 sun,
                                                  f32 wind_speed, f32 shelter_visibility,
                                                  f32 seconds) noexcept;

/// What one accumulation pass did.
struct AccumulationReport {
    u64 lattice_points = 0;
    /// Points whose wetness rose, and points whose snow rose. A diagnostic, and the two numbers a
    /// suite watching "it rained and the ground got wet" asserts on.
    u64 wetted = 0;
    u64 snowed = 0;
    u64 dried = 0;
    u64 melted = 0;
};

/// Accumulates precipitation into the two fields, and decays them.
///
/// It reads the current field values back through the store, steps them, and writes them again:
/// **the field IS the state.** There is no shadow copy here to keep in step with the tiles, which
/// is what makes "a region that was snowed on and unloaded retains its snow state" a consequence of
/// the substrate's persistence rather than of a second record.
class Accumulation {
public:
    explicit Accumulation(Allocator& allocator) noexcept;

    Accumulation(const Accumulation&) = delete;
    Accumulation& operator=(const Accumulation&) = delete;

    void set_model(const AccumulationModel& model) noexcept { model_ = model; }
    [[nodiscard]] const AccumulationModel& model() const noexcept { return model_; }
    /// The occlusion representation, for the sun and shelter terms. Optional: a world with no
    /// cover gets open sky everywhere, which is correct rather than degenerate.
    void set_occlusion(const SkyOcclusion* occlusion) noexcept { occlusion_ = occlusion; }

    /// Step the two fields over a region by `seconds` of simulated time.
    ///
    /// `shore` is the water row's `water-shore-wetness` field, or an invalid identity when this
    /// configuration's water row owns nothing. When it is valid the published wetness is the
    /// MAXIMUM of the shore's contribution and the accumulated one — see fields.h's header note.
    [[nodiscard]] Expected<AccumulationReport, Error> advance(WeatherFields& fields,
                                                              const EnvironmentSampler& sampler,
                                                              const PublishRegion& region,
                                                              f32 seconds) noexcept;

private:
    /// One lattice point, staged into both fields. Split out because the tile arithmetic and the
    /// model step are different things to read, and the first is what a reader checks against
    /// `environment::store.cpp`'s own lattice convention.
    [[nodiscard]] Status step_point(environment::FieldWriter& wetness,
                                    environment::FieldWriter& snow, WeatherFields& fields,
                                    const EnvironmentSampler& sampler,
                                    environment::FieldResidency level, i64 lattice_x, i64 lattice_z,
                                    f32 cell_metres, f32 seconds,
                                    AccumulationReport& report) noexcept;

    Allocator* allocator_;
    AccumulationModel model_;
    const SkyOcclusion* occlusion_ = nullptr;
};

}  // namespace cy::weather
