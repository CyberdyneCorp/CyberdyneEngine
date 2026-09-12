#pragma once
// CYBERWEATHER, assembled: the climate, the cells, the wind, the storms, the presets, the
// accumulation and the ecosystem, under one tick and one set of budgets. M10 tasks 3.1 and 3.2.
//
// ================================================================================================
// WHAT THIS CLASS IS NOT
// ================================================================================================
//
// `weather-and-wind` forbids "a central weather component that pushes state to subsystems", so it
// is worth being exact about what `WeatherSystem` is: it is a COMPOSITION ROOT and a TICK. It owns
// the models, it advances them in a declared order, and it publishes their output into
// `environment-fields`. It holds no list of consumers, no callback into one, and no method that
// takes a material, a foliage instance, a water body or a particle system. Everything downstream
// reads fields.
//
// ================================================================================================
// THE DETERMINISM DECLARATION, STATED ONCE, HERE
// ================================================================================================
//
// "Weather SHALL declare which of its state is AUTHORITATIVE — participating in gameplay and
// therefore deterministic or server-authoritative — and which is VISUAL DETAIL."
//
//   AUTHORITATIVE   `WeatherState` in every cell; every storm in `StormRegistry`; the transition in
//                   `WeatherDirector`; the wind's `base` and `gust`; the wetness, snow, and
//                   ecosystem fields. All of it evolves from (session seed, tick, recorded events)
//                   and nothing else — no frame timing, no camera, no wall clock — which is what
//                   `advance()` taking a `SimulationPoint` and a fixed step is for.
//   VISUAL DETAIL   the wind's `turbulence` residual, and the precipitation PLAN. Both are computed
//                   FROM authoritative state and read back by nothing in this module;
//                   `PresentationLevers` moves them and reaches nothing else.
//
// **The measurement, in `test_determinism.cpp`:** two systems seeded identically, one running at
// the lowest presentation levers and one at the highest, ticked in lockstep for a thousand ticks,
// produce bit-identical authoritative state and bit-identical authoritative wind — which is the
// "Particles do not affect gameplay" and "Cost is bounded, state is not degraded" scenarios
// measured rather than asserted.
//
// ================================================================================================
// THE SKY IS A CONSUMER, AND IT IS NOT LINKED
// ================================================================================================
//
// `atmosphere-sky-and-clouds` declares `rendering::sky::CloudWeatherState` and says in its own
// header that `weather-and-wind` produces it. This module produces `CloudDrive` — the same six
// quantities, in the same units — and the composition point (which already links both) assigns them
// field by field. Linking the renderer from here to name one struct would put presentation
// underneath authority and would make weather uncookable; `sky_light.h`'s three-line GI adapter is
// the precedent, and this is the same shape.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/memory/array.h>
#include <cy/weather/accumulation.h>
#include <cy/weather/cells.h>
#include <cy/weather/climate.h>
#include <cy/weather/ecosystem.h>
#include <cy/weather/fields.h>
#include <cy/weather/precipitation.h>
#include <cy/weather/preset.h>
#include <cy/weather/sample.h>
#include <cy/weather/storm.h>
#include <cy/weather/wind.h>

namespace cy::weather {

/// What `atmosphere-sky-and-clouds` consumes. See the header note: this is the producer half of
/// `rendering::sky::CloudWeatherState`, in a struct that does not require this module to link the
/// renderer.
struct CloudDrive {
    /// The wind at the cloud reference altitude, m/s, world axes.
    Vec3 wind;
    /// How much faster the wind blows per kilometre of altitude.
    f32 shear_per_km = 0.15F;
    f32 humidity = 0.5F;
    /// [0, 1]: the strongest storm influence at the reference position.
    f32 storm_intensity = 0.0F;
    /// Millimetres per hour. Gameplay-visible, and identical at every rendering tier.
    f32 precipitation_mm_per_hour = 0.0F;
    f32 temperature_celsius = 15.0F;
    /// Bumped whenever any of the above changes, for a consumer's history rejection.
    u32 epoch = 0;
};

/// The budgets `weather-and-wind` requires each part to hold. "Weather simulation, field updates,
/// cloud rendering, volumetrics, and precipitation presentation SHALL EACH hold declared budgets:
/// processor time, GPU time, field memory, and field update bandwidth."
///
/// The two halves are separated in the type, not only in a comment: `SimulationBudget` bounds the
/// authoritative evolution and is independent of the view; `PresentationLevers` (precipitation.h)
/// and `presentation` below are what a renderer's budget arbiter moves, and nothing in this module
/// reads them back.
struct SimulationBudget {
    /// Milliseconds of processor time the weather evolution may take per tick. Reported against,
    /// not enforced by pre-emption: what bounds the cost is the grid, and this is the number a
    /// profiler view compares the grid's cost to.
    f32 simulation_ms = 0.8F;
    /// Milliseconds for the field publication.
    f32 field_update_ms = 1.5F;
    /// Bytes of field memory weather's own fields may occupy.
    u64 field_memory_bytes = 64ULL * 1024ULL * 1024ULL;
    /// Lattice points published per tick. The field update BANDWIDTH, and the one lever that
    /// actually throttles: a publication that would exceed it covers less world this tick and the
    /// rest next tick.
    u64 field_points_per_tick = 262'144;
};

/// What one tick did, at every level. Everything a profiler view and a suite need.
struct WeatherTickReport {
    StepReport cells;
    PublishReport atmosphere;
    AccumulationReport accumulation;
    EcosystemReport ecosystem;
    u32 lightning_events = 0;
    u32 storms = 0;
    /// Publication deferred because `field_points_per_tick` was reached.
    bool publication_throttled = false;
};

/// How the system is set up. Everything that must be decided before the first tick, in one struct,
/// so a project configures weather in one call and a test configures it in three lines.
struct WeatherConfig {
    WeatherGridConfig grid;
    WeatherFieldOptions fields;
    OrographicModel orographic;
    GustModel gust;
    TurbulenceModel turbulence;
    AccumulationModel accumulation;
    EcosystemModel ecosystem;
    SimulationBudget budget;
    PrecipitationLevers presentation;
    /// The session seed. Every draw this module makes is a substream of it.
    u64 seed = 0x5745'4154'4845'5200ULL;  // "WEATHER\0"
    /// Ticks per second, for transitions and for the simulated clock.
    f64 ticks_per_second = 60.0;
    /// The altitude the cloud drive reports the wind at, metres.
    f32 cloud_reference_altitude_metres = 2000.0F;
};

/// CyberWeather. See the header note for what it is and is not.
class WeatherSystem {
public:
    explicit WeatherSystem(Allocator& allocator) noexcept;

    WeatherSystem(const WeatherSystem&) = delete;
    WeatherSystem& operator=(const WeatherSystem&) = delete;

    /// Size everything and seed the cells from the climate. The climate is borrowed and must
    /// outlive the system — it is authored data, and copying a world's climate grid into the
    /// simulation would be a second copy to keep in step.
    [[nodiscard]] Status configure(const WeatherConfig& config, const ClimateMap& climate) noexcept;

    /// Declare and claim the field set. Separate from `configure()` because a project may want the
    /// model without the fields — the editor's climate preview does — and because this is where a
    /// second producer is refused and a caller wants that refusal by itself.
    [[nodiscard]] Status bind_fields(environment::FieldRegistry& registry,
                                     environment::FieldStore& store) noexcept;

    void set_terrain(const TerrainProfile& terrain) noexcept;
    void set_occlusion(const SkyOcclusion* occlusion) noexcept;
    /// The region published each tick. The caller's, for the reason fields.h gives.
    void set_publish_region(const PublishRegion& region) noexcept { publish_ = region; }
    [[nodiscard]] const PublishRegion& publish_region() const noexcept { return publish_; }

    /// One tick. The declared order: presets, storms, cells, wind, publication, accumulation,
    /// ecosystem — each of which reads the output of the ones before it and none of which reads the
    /// output of one after.
    [[nodiscard]] Expected<WeatherTickReport, Error> advance(determinism::SimulationPoint at,
                                                             f64 seconds) noexcept;

    /// Advance the macro state by days without ticking the atmosphere: the editor's fast-forward.
    /// It runs the SAME `Ecosystem::advance()` and the SAME `Accumulation::advance()` the tick runs
    /// — see ecosystem.h — so the preview and the runtime cannot disagree.
    [[nodiscard]] Expected<WeatherTickReport, Error> advance_days(determinism::SimulationPoint at,
                                                                  f64 days) noexcept;

    // --- Reading
    // ----------------------------------------------------------------------------------

    [[nodiscard]] const EnvironmentSampler& sampler() const noexcept { return sampler_; }
    [[nodiscard]] EnvironmentSample sample(const world::WorldVec3d& at, SampleQuality quality,
                                           determinism::SimulationClass reader) const noexcept;
    [[nodiscard]] CloudDrive cloud_drive(const world::WorldVec3d& at) const noexcept;
    [[nodiscard]] PrecipitationPlan precipitation_plan(const world::WorldVec3d& at) const noexcept;

    [[nodiscard]] WeatherCells& cells() noexcept { return cells_; }
    [[nodiscard]] const WeatherCells& cells() const noexcept { return cells_; }
    [[nodiscard]] WindComposer& wind() noexcept { return wind_; }
    [[nodiscard]] const WindComposer& wind() const noexcept { return wind_; }
    [[nodiscard]] StormRegistry& storms() noexcept { return storms_; }
    [[nodiscard]] const StormRegistry& storms() const noexcept { return storms_; }
    [[nodiscard]] WeatherDirector& director() noexcept { return director_; }
    [[nodiscard]] const WeatherDirector& director() const noexcept { return director_; }
    [[nodiscard]] WeatherFields& fields() noexcept { return fields_; }
    [[nodiscard]] const WeatherFields& fields() const noexcept { return fields_; }
    [[nodiscard]] Ecosystem& ecosystem() noexcept { return ecosystem_; }
    [[nodiscard]] Accumulation& accumulation() noexcept { return accumulation_; }
    [[nodiscard]] const ClimateMap* climate() const noexcept { return climate_; }
    [[nodiscard]] const SkyOcclusion* occlusion() const noexcept { return occlusion_; }
    [[nodiscard]] const WeatherConfig& config() const noexcept { return config_; }
    [[nodiscard]] u64 tick() const noexcept { return tick_; }
    [[nodiscard]] f64 seconds() const noexcept { return seconds_; }
    /// The simulated moment the last `advance()` ran at. What a diagnostic re-samples the wind at,
    /// so that an inspection reports the wind the tick produced rather than one from a moment
    /// nothing was simulated at.
    [[nodiscard]] determinism::SimulationPoint moment() const noexcept { return moment_; }

    // --- Presentation levers
    // ----------------------------------------------------------------------
    //
    // The one place a quality tier reaches. Reducing them changes the precipitation PLAN and
    // nothing else; see the header note and `test_determinism.cpp`.

    void set_presentation(const PrecipitationLevers& levers) noexcept { presentation_ = levers; }
    [[nodiscard]] const PrecipitationLevers& presentation() const noexcept { return presentation_; }

    // --- Replication and replay
    // -------------------------------------------------------------------
    //
    // "Networking SHALL replicate LOW-FREQUENCY AUTHORITATIVE STATE: storm identity, position,
    // velocity, intensity, regional weather state, and events such as lightning. Clients SHALL
    // reconstruct visual detail locally." And "Replays SHALL reconstruct weather from its SEED AND
    // DETERMINISTIC EVOLUTION, or from recorded authoritative state deltas and events."

    /// The authoritative snapshot: the storms, the global tendency, the director's transition and
    /// the regional grid. Appended to `out`, which is not cleared.
    [[nodiscard]] Status encode_state(Array<u8>& out) const noexcept;
    /// Apply a snapshot. The receiving system must already be `configure()`d with the same grid —
    /// a snapshot carries state, not shape, which is what keeps its size proportional to the world
    /// rather than to the world plus its configuration.
    [[nodiscard]] Status decode_state(Span<const u8> bytes) noexcept;
    /// Bytes `encode_state()` would write. A function of the grid alone, so a project can budget
    /// its replication before it has any weather.
    [[nodiscard]] usize encoded_state_bytes() const noexcept;

private:
    [[nodiscard]] Status publish(WeatherTickReport& report) noexcept;

    Allocator* allocator_;
    WeatherConfig config_;
    const ClimateMap* climate_ = nullptr;
    const SkyOcclusion* occlusion_ = nullptr;

    WeatherCells cells_;
    WindComposer wind_;
    StormRegistry storms_;
    WeatherDirector director_;
    WeatherFields fields_;
    Accumulation accumulation_;
    Ecosystem ecosystem_;
    EnvironmentSampler sampler_;

    PublishRegion publish_;
    PrecipitationLevers presentation_;
    Array<LightningEvent> lightning_;

    determinism::SimulationPoint moment_;
    u64 tick_ = 0;
    f64 seconds_ = 0.0;
    u32 cloud_epoch_ = 0;
    bool configured_ = false;
};

}  // namespace cy::weather
