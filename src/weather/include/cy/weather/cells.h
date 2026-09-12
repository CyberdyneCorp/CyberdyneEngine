#pragma once
// WEATHER CELLS: the current state, the hierarchy over it, and the two things that move it —
// advection with the wind, and the terrain the wind has to climb. M10 task 3.1.
//
// `weather-and-wind` — "Weather cells and hierarchy":
//
//   * state "SHALL be maintained over weather cells at a resolution appropriate to the world,
//     typically kilometres, refined near streaming sources";
//   * weather "SHALL be hierarchical: global and continental tendencies inform regional
//     conditions, which inform local conditions, which are modified by terrain";
//   * "Weather cells SHALL NOT be required to align with world partition cells, PCG regions, or
//     field tiles; each system's partition serves its own purpose";
//   * "Full computational meteorology SHALL NOT be attempted. The model SHALL be plausible and
//     controllable: advection of state with wind, storm phenomena, and terrain influence such as
//     rain shadow and orographic effects."
//
// ================================================================================================
// THE NON-ALIGNMENT IS A MEASUREMENT, NOT A COMMENT
// ================================================================================================
//
// "SHALL NOT be required to align" is a sentence that passes trivially for any implementation that
// happens to align. So this grid carries its OWN origin and its OWN cell size, neither derived from
// `world::PartitionConfig`, and `aligns_with()` reports whether a given configuration happens to
// line up. The default configuration deliberately does not: 4 000 m cells offset by a prime number
// of metres against 128 m world cells, so that every cell boundary in one partition falls inside a
// cell of the other. `test_cells.cpp` runs the whole evolution on that configuration and samples
// across a world-cell boundary; a module that had quietly assumed alignment fails there.
//
// ================================================================================================
// COST IS BOUNDED BY THE GRID, NOT BY THE CAMERA
// ================================================================================================
//
// "Simulation budgets SHALL bound weather evolution cost independently of view, so that a large
// world's weather cost does not scale with what is on screen", and "weather simulation cost SHALL
// remain bounded by its own budget" as the camera moves.
//
// The regional grid covers the whole world and every cell of it is stepped every macro step,
// whether or not anything is looking. Refinement is the only view-dependent part and it is
// BOUNDED — `max_local_cells` — so the worst case is grid + budget and the camera cannot raise it.
// `StepReport::cells_stepped` is that number counted, and a suite walks a camera a thousand
// kilometres and requires it not to move.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/weather/climate.h>
#include <cy/world/coordinates.h>

namespace cy::weather {

class StormRegistry;

/// What falls out of the sky. `environment-fields` stores it as a `Category`, which is why it is an
/// index and not a bit set: at a position there is one kind of precipitation, and a cell that was
/// rain and snow at once would be a cell whose interpolated type names neither.
enum class PrecipitationType : u8 {
    None = 0,
    Rain = 1,
    Snow = 2,
    Hail = 3,
    Ash = 4,
    Dust = 5,
    /// The first index a project may define. Nothing in the engine switches on values at or above
    /// it, and `precipitation.h`'s presentation table carries a project row for them.
    Project = 6,
};

[[nodiscard]] const char* precipitation_type_name(PrecipitationType type) noexcept;

/// The current environmental state of one cell: the specification's middle row, member for member.
///
/// EVERY MEMBER IS AUTHORITATIVE. Visibility gates detection, wind carries projectiles,
/// precipitation slows movement, temperature decides whether water freezes — so this struct is
/// `simulation-and-determinism`'s `Authoritative` class and is what the determinism profile applies
/// to. The presentation half of weather is elsewhere and by construction: cloud voxels, particles
/// and the turbulence residual in wind.h, none of which is reachable from here.
struct WeatherState {
    f32 temperature_celsius = 14.0F;
    /// Relative humidity, [0, 1].
    f32 humidity = 0.6F;
    /// Station pressure, hectopascals. A storm's low is a depression in this.
    f32 pressure_hpa = 1013.25F;
    /// Horizontal wind, m/s, world axes: x in `.x`, z in `.y`.
    Vec2 wind{4.0F, 0.0F};
    /// Vertical wind, m/s, positive up. Orographic uplift lives here, and it is what turns humidity
    /// into rain on the windward slope.
    f32 vertical_wind = 0.0F;
    f32 cloud_coverage = 0.25F;
    f32 precipitation_mm_per_hour = 0.0F;
    PrecipitationType precipitation_type = PrecipitationType::None;
    /// Metres. Fog, rain and dust all reduce it, and artificial intelligence reads it for
    /// detection.
    f32 visibility_metres = 20'000.0F;

    friend bool operator==(const WeatherState&, const WeatherState&) noexcept = default;
};

/// Which level of the hierarchy an answer came from. Ordered coarsest first, so a walk downward is
/// a walk toward detail and the enumerator's value IS the level index.
enum class WeatherScale : u8 {
    /// One state for the whole world: the continental tendency.
    Global = 0,
    /// The grid that covers the world. Always resident; this is the level a macro sample reads.
    Regional,
    /// Refined cells near a refinement source. Present only where something asked for them.
    Local,
    kCount,
};

inline constexpr u32 kWeatherScaleCount = static_cast<u32>(WeatherScale::kCount);

[[nodiscard]] const char* weather_scale_name(WeatherScale scale) noexcept;

/// Ground elevation at a horizontal position, in metres above sea level.
///
/// THE TERRAIN SEAM, and the reason `cy::terrain` is absent from this module's link line. A world
/// with no terrain installs nothing and gets a flat world — no rain shadow, no uplift, no
/// channelling — which is the correct answer for a flat world rather than a degenerate one.
struct TerrainProfile {
    f64 (*elevation_at)(void* user, f64 x, f64 z) noexcept = nullptr;
    void* user = nullptr;

    [[nodiscard]] bool installed() const noexcept { return elevation_at != nullptr; }
    [[nodiscard]] f64 elevation(f64 x, f64 z) const noexcept {
        return installed() ? elevation_at(user, x, z) : 0.0;
    }
};

/// The declared orographic model. Named constants rather than magic numbers inside the step,
/// because "from a DECLARED model rather than hand-painted" is the requirement's own wording and a
/// designer tuning a rain shadow needs somewhere to turn the dial.
struct OrographicModel {
    /// Metres of vertical wind per (metre of rise per metre travelled) per m/s of horizontal wind.
    /// Uplift is the wind speed times the slope along it; this scales that product.
    f32 uplift_gain = 1.0F;
    /// Millimetres per hour of rain added per m/s of uplift, at saturation humidity.
    f32 orographic_rain_gain = 3.0F;
    /// Fraction of a cell's humidity removed per millimetre per hour of orographic rain. This is
    /// what makes the LEEWARD side dry: the air that climbed the range arrives having rained.
    f32 humidity_loss_per_mm = 0.06F;
    /// Fraction of the leeward rain suppressed per m/s of DOWNWARD vertical wind. Descending air
    /// warms and its relative humidity falls, and the shadow is longer than the slope.
    f32 rain_shadow_gain = 0.25F;
    /// Degrees Celsius lost per kilometre of elevation, applied between the grid's reference
    /// altitude and the ground.
    f32 lapse_rate_per_km = 6.5F;
    /// How much a ridge accelerates the wind crossing it, per (metre of rise per metre). Ridge
    /// acceleration and lee blocking are the same coefficient with the sign of the slope.
    f32 ridge_speed_gain = 0.8F;
};

/// The grid's own geometry. Nothing here is derived from `world::PartitionConfig`; see the header.
struct WeatherGridConfig {
    /// The world position of regional cell (0, 0)'s centre.
    f64 origin_x = -511.0;
    f64 origin_z = -257.0;
    /// Metres per regional cell. "Typically kilometres", and four is the scale at which a shower
    /// is one cell.
    f32 regional_cell_metres = 4000.0F;
    u32 width = 32;
    u32 height = 32;
    /// How many local cells one regional cell splits into per axis.
    u32 refine_ratio = 8;
    /// The refinement budget. The whole of the view-dependent cost; see the header note.
    u32 max_local_cells = 4096;
    /// Seconds of simulated time in one macro step. The grid advances in whole steps of this, and
    /// nothing else: it is what makes `advance(90 days)` and ninety `advance(1 day)` calls produce
    /// the same state bit for bit — see `advance()`.
    f32 macro_step_seconds = 60.0F;
    /// How fast a cell relaxes toward its target: the fraction of the remaining gap closed per
    /// second, applied exponentially so the result does not depend on the step size.
    f32 relaxation_per_second = 0.0015F;

    [[nodiscard]] bool is_valid() const noexcept;
    /// Whether this grid happens to line up with a world partition — same cell size or an integer
    /// ratio of it, and a coincident origin. Reported rather than required: see the header note.
    [[nodiscard]] bool aligns_with(const world::PartitionConfig& partition) const noexcept;
};

/// What one call to `advance()` did. Counted, not estimated, because three requirements are about
/// these numbers: the refinement budget, the view independence of cost, and the simulation budget.
struct StepReport {
    /// Whole macro steps run. Zero for a call whose seconds did not fill one.
    u32 steps = 0;
    /// Cell updates performed, summed over the steps. Regional cells times steps, plus local.
    u64 cells_stepped = 0;
    /// Local cells resident after the call.
    u32 local_cells = 0;
    /// Refinements the budget refused. A number a project tunes against, not an error.
    u32 refinements_dropped = 0;
    /// Simulated seconds consumed by the whole steps. The remainder is carried.
    f64 seconds_advanced = 0.0;
};

/// A place that wants fine weather. "refined near streaming sources where higher fidelity is
/// needed" — a camera, a player, an audio listener; whatever the application declares.
struct RefinementSource {
    f64 x = 0.0;
    f64 z = 0.0;
    f32 radius_metres = 2000.0F;
    /// Higher wins when the budget cannot satisfy every source. Ties break on the source's index,
    /// which is stable, so a budget that refuses is deterministic about what it refused.
    u16 priority = 0;
};

/// One sampled answer, with the resolution indicator the specification requires.
struct WeatherCellSample {
    WeatherState state;
    WeatherScale scale = WeatherScale::Regional;
    f32 cell_metres = 0.0F;
    /// False when the position lay outside the grid and `state` is the global tendency. The grid
    /// saturates rather than defaulting: a world's weather does not stop at the edge of its map.
    bool inside_grid = true;
};

/// The hierarchy, the state it holds, and the step that moves it.
///
/// It owns no fields and writes none: `WeatherFields` (fields.h) is what publishes this state into
/// `environment-fields`. The split is deliberate — this is the model, that is the publication, and
/// a class that did both would make "weather publishes state and touches nothing" a property of a
/// method rather than of the module.
class WeatherCells {
public:
    explicit WeatherCells(Allocator& allocator) noexcept;

    WeatherCells(const WeatherCells&) = delete;
    WeatherCells& operator=(const WeatherCells&) = delete;

    /// Size the grid and seed every cell from the climate. Refuses an invalid configuration.
    [[nodiscard]] Status configure(const WeatherGridConfig& config, const ClimateMap& climate,
                                   u64 seed) noexcept;

    void set_terrain(const TerrainProfile& terrain) noexcept { terrain_ = terrain; }
    void set_orographic_model(const OrographicModel& model) noexcept { orographic_ = model; }
    /// The storms contributing to the state. Borrowed, not owned; may be null.
    void set_storms(const StormRegistry* storms) noexcept { storms_ = storms; }

    [[nodiscard]] Status set_refinement_sources(Span<const RefinementSource> sources) noexcept;

    /// The target the whole grid relaxes toward, over and above its climate. This is what a preset
    /// transition drives (preset.h): a director sets the target, and the cells arrive at it on
    /// their own time constants rather than switching.
    void set_global_target(const WeatherState& target) noexcept { global_target_ = target; }
    [[nodiscard]] const WeatherState& global_target() const noexcept { return global_target_; }
    [[nodiscard]] const WeatherState& global_state() const noexcept { return global_; }

    /// Replace the state wholesale. What a REPLICATION SNAPSHOT and a save restore do, and the only
    /// path in the module that sets a cell rather than moving it — which is why the two are named
    /// for it rather than hidden behind `sample()`'s neighbourhood.
    void set_global_state(const WeatherState& state) noexcept { global_ = state; }
    [[nodiscard]] Status set_regional_cell(u32 index, const WeatherState& state) noexcept;

    /// Advance by `seconds` of simulated time, in whole macro steps.
    ///
    /// **The remainder is carried, and that is what makes the editor's fast-forward the runtime's
    /// own model.** `advance(90 days)` runs exactly as many steps as ninety `advance(1 day)` calls
    /// and reaches the same state bit for bit, because both are the same loop over the same step.
    /// "It SHALL use the same macro state and generation path as the runtime, not a separate
    /// preview model" is therefore a property of the code rather than a claim about it —
    /// `test_ecosystem.cpp` compares the two.
    [[nodiscard]] Expected<StepReport, Error> advance(determinism::SimulationPoint at,
                                                      f64 seconds) noexcept;

    [[nodiscard]] WeatherCellSample sample(f64 x, f64 z, WeatherScale finest) const noexcept;
    [[nodiscard]] WeatherCellSample sample(const world::WorldVec3d& at,
                                           WeatherScale finest) const noexcept;

    [[nodiscard]] const WeatherGridConfig& config() const noexcept { return config_; }
    [[nodiscard]] u32 regional_cells() const noexcept { return config_.width * config_.height; }
    [[nodiscard]] usize local_cells() const noexcept { return local_.size(); }
    [[nodiscard]] u64 macro_steps() const noexcept { return macro_steps_; }
    /// The uplift the orographic model computed at a position, m/s. Exposed for the inspector's
    /// "why is it raining here" answer and for the suite that measures the rain shadow.
    [[nodiscard]] f32 uplift_at(f64 x, f64 z, const Vec2& wind) const noexcept;

    /// Regional cell indices, for a diagnostic that draws the grid.
    [[nodiscard]] i32 cell_x_of(f64 x) const noexcept;
    [[nodiscard]] i32 cell_z_of(f64 z) const noexcept;
    [[nodiscard]] const WeatherState* regional_cell(i32 i, i32 k) const noexcept;

    /// The climate SEEDED INTO a cell, read back without touching the climate map.
    ///
    /// This is what makes "climate SHALL NOT be recomputed to answer a question about the current
    /// state" hold for the wind composition too: `WindComposer`'s prevailing term reads THIS, not
    /// `ClimateMap::sample()`, so a frame of wind sampling leaves `ClimateMap::evaluations()`
    /// exactly where it found it. A position outside the grid gets the nearest cell's climate, for
    /// the reason `ClimateMap::fetch()` saturates.
    [[nodiscard]] ClimateSample climate_at(f64 x, f64 z) const noexcept;

private:
    /// One local cell, keyed by its own coordinate on the refined lattice.
    struct LocalCell {
        i32 i = 0;
        i32 k = 0;
        WeatherState state;
    };

    [[nodiscard]] Status step_once(determinism::SimulationPoint at, f32 dt,
                                   StepReport& report) noexcept;
    /// The global tendency: one state relaxing toward the target and the storms.
    void step_global(f32 dt) noexcept;
    /// Every regional cell toward its climate-and-global target, then the storms, then terrain.
    void step_regional(f32 dt) noexcept;
    /// The target one cell relaxes toward. Split out because it is the whole of the hierarchy —
    /// "global and continental tendencies inform regional conditions" — and a reader checking that
    /// sentence should not have to read a relaxation loop to find it.
    [[nodiscard]] WeatherState regional_target(const WeatherState& climate) const noexcept;
    /// The storms' contribution to one cell's TARGET. See the definition for why it is the target
    /// and not the state — a contribution added after every relaxation is amplified by the
    /// reciprocal of the relaxation fraction.
    void apply_storms(u32 index, WeatherState& target) const noexcept;
    /// Semi-Lagrangian advection of the transported quantities. Separate because it is the one step
    /// that reads the WHOLE previous grid and writes a new one, and mixing it into the relaxation
    /// would make the relaxation order-dependent.
    void advect(f32 dt) noexcept;
    /// Terrain's modification of a cell's TARGET: uplift, orographic rain, the lee shadow and the
    /// lapse rate. The target for the same reason `apply_storms()` takes one.
    void apply_terrain(u32 index, WeatherState& target) const noexcept;
    /// Rebuild the local lattice from the refinement sources, within the budget.
    [[nodiscard]] Status refine(StepReport& report) noexcept;
    /// One candidate local cell. See the definition: the circle test and the budget live here.
    [[nodiscard]] Status place_local(const RefinementSource& source, i64 i, i64 k,
                                     StepReport& report) noexcept;
    /// One local cell's state: its parent regional cell, modified by terrain at the finer scale.
    [[nodiscard]] WeatherState local_state_of(i32 i, i32 k) const noexcept;

    [[nodiscard]] WeatherState climate_target(u32 index) const noexcept;
    [[nodiscard]] const LocalCell* find_local(i32 i, i32 k) const noexcept;

    Allocator* allocator_;
    WeatherGridConfig config_;
    OrographicModel orographic_;
    TerrainProfile terrain_;
    const StormRegistry* storms_ = nullptr;

    /// The climate seeded into each regional cell at `configure()`, kept so that the step does not
    /// reach back into the climate map. See climate.h's header note.
    Array<ClimateSample> climate_;
    Array<WeatherState> regional_;
    /// The advection's read buffer. A member so a step allocates nothing.
    Array<WeatherState> scratch_;
    Array<LocalCell> local_;
    HashMap<u64, usize> local_index_;
    Array<RefinementSource> sources_;

    WeatherState global_;
    WeatherState global_target_;
    u64 seed_ = 0;
    u64 macro_steps_ = 0;
    /// Simulated seconds not yet consumed by a whole macro step. See `advance()`.
    f64 carry_ = 0.0;
    bool configured_ = false;
};

}  // namespace cy::weather
