#pragma once
// THE ENVIRONMENT SAMPLE: one answer at one position, at a declared quality. M10 task 3.1.
//
// `weather-and-wind` — "Environment sampling": the engine "SHALL provide an environment sample at a
// world position: temperature, humidity, pressure, wind, precipitation rate by type, cloud
// coverage, visibility, and derived values such as wetness potential"; "Sampling SHALL declare a
// QUALITY: macro for strategic reasoning, gameplay for simulation, and high-frequency for effects
// near the camera — so that artificial intelligence does not pay for detail it cannot use";
// "Sampling SHALL be batchable and SHALL NOT allocate"; "Sampling in a region whose fine data is
// not resident SHALL return the coarsest resident value with a RESOLUTION INDICATOR, and SHALL NOT
// block."
//
// ================================================================================================
// QUALITY IS A COST DIAL, NOT A DIFFERENT ANSWER
// ================================================================================================
//
// The three qualities differ in WHICH TERMS ARE EVALUATED, and each finer one is the coarser one
// plus a term:
//
//   Macro            the regional weather cell, and nothing else. No storms resolved individually,
//                    no terrain re-evaluation, no wind volumes, no gust. What an agent evaluating a
//                    hundred positions across a map needs, and it costs one grid lookup each.
//   Gameplay         the above, plus every storm's own contribution, terrain influence and the
//                    authored and transient wind volumes, plus the deterministic gust. What the
//                    simulation reads: it is the authoritative answer, and it is the one a replay
//                    reproduces.
//   HighFrequency    the above, plus the local refined weather cell where one exists and the
//                    presentation-only turbulence residual. PRESENTATION: a sample at this quality
//                    carries state an authoritative reader may not read, so `sample()` refuses it
//                    to one — see `EnvironmentSampler::sample()`.
//
// The important property, and the one `test_sampling.cpp` measures: **a macro sample and a gameplay
// sample never disagree about a quantity the macro one reports.** Macro is not an approximation
// with its own error; it is the same weather cell read with fewer terms added on top. An agent
// reasoning at macro quality and a simulation acting at gameplay quality therefore cannot reach
// contradictory conclusions about which valley is raining.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/classification.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/math/vec.h>
#include <cy/weather/cells.h>
#include <cy/weather/wind.h>
#include <cy/world/coordinates.h>

namespace cy::weather {

/// What the caller is willing to pay for. See the header note.
enum class SampleQuality : u8 { Macro = 0, Gameplay, HighFrequency };

[[nodiscard]] const char* sample_quality_name(SampleQuality quality) noexcept;

/// One environment sample: every quantity the specification's list names, plus the derived ones it
/// asks for and the resolution indicator it requires.
struct EnvironmentSample {
    f32 temperature_celsius = 14.0F;
    f32 humidity = 0.6F;
    f32 pressure_hpa = 1013.25F;
    WindSample wind;
    f32 precipitation_mm_per_hour = 0.0F;
    PrecipitationType precipitation_type = PrecipitationType::None;
    f32 cloud_coverage = 0.25F;
    f32 visibility_metres = 20'000.0F;

    /// DERIVED: how wet this place tends to become under the current conditions, [0, 1]. It is not
    /// the wetness field — that is accumulated state with a history, and this is the equilibrium
    /// the accumulation is heading for. `accumulation.h` uses exactly this number as its target,
    /// which is what keeps the two from drifting apart.
    f32 wetness_potential = 0.0F;
    /// DERIVED: whether precipitation at this position is frozen, from temperature and type.
    bool freezing = false;

    // --- The resolution indicator
    // ------------------------------------------------------------------
    //
    // "SHALL return the coarsest resident value with a resolution indicator, and SHALL NOT block."

    /// The quality actually delivered. Lower than requested where the finer data did not exist, and
    /// never higher.
    SampleQuality quality = SampleQuality::Macro;
    /// The level of the hierarchy the state came from.
    WeatherScale scale = WeatherScale::Regional;
    /// Metres per cell of that level. What a consumer deciding "is this precise enough" compares.
    f32 cell_metres = 0.0F;
    /// False when the position lies outside the configured grid and the global tendency answered.
    bool inside_grid = true;
};

/// Sampling, at a quality, for a reader of a declared class.
///
/// It borrows everything and owns nothing: the cells, the wind composer and the seconds clock all
/// belong to `WeatherSystem`. A sampler is therefore free to be copied, held per worker, and used
/// from several threads — none of its methods writes anything, and the two it calls do not either.
class EnvironmentSampler {
public:
    EnvironmentSampler() = default;
    EnvironmentSampler(const WeatherCells& cells, const WindComposer& wind) noexcept
        : cells_(&cells), wind_(&wind) {}

    /// The simulated moment the sample is taken at. Set once per tick by `WeatherSystem`.
    void set_moment(determinism::SimulationPoint when, f64 seconds) noexcept {
        when_ = when;
        seconds_ = seconds;
    }

    /// One sample.
    ///
    /// A `HighFrequency` request from an authoritative reader is DOWNGRADED to `Gameplay` rather
    /// than refused: the reader asked for detail it may not have, and the honest answer is the
    /// authoritative one with `quality` saying so. Refusing would make a perfectly legal question
    /// an error; silently answering with presentation state would be the firewall crossing itself.
    [[nodiscard]] EnvironmentSample sample(const world::WorldVec3d& at, SampleQuality quality,
                                           determinism::SimulationClass reader) const noexcept;

    /// "Sampling SHALL be batchable and SHALL NOT allocate." `out` is the caller's; this writes
    /// into it and allocates nothing, which is why it takes a `Span` rather than an `Array`.
    [[nodiscard]] Status sample_many(Span<const world::WorldVec3d> positions, SampleQuality quality,
                                     determinism::SimulationClass reader,
                                     Span<EnvironmentSample> out) const noexcept;

    [[nodiscard]] bool bound() const noexcept { return cells_ != nullptr && wind_ != nullptr; }

private:
    const WeatherCells* cells_ = nullptr;
    const WindComposer* wind_ = nullptr;
    determinism::SimulationPoint when_;
    f64 seconds_ = 0.0;
};

/// The equilibrium wetness the current conditions imply, [0, 1]. Free, so that `accumulation.h`'s
/// target and the sample's derived value are one function rather than two that agree today.
[[nodiscard]] f32 wetness_potential_of(const WeatherState& state) noexcept;

}  // namespace cy::weather
