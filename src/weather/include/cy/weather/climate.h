#pragma once
// CLIMATE: the slow layer. M10 task 3.1.
//
// `weather-and-wind` — "Climate, weather, and presentation": three layers, and the table that
// separates them is this module's spine.
//
//   | Climate      | prevailing tendencies: mean temperature, humidity, prevailing wind, rainfall
//   |              | potential, solar exposure, ocean influence | changes rarely; authored or
//   |              | generated |
//
// And two sentences that are requirements rather than description:
//
//   * "climate SHALL NOT be recomputed to answer a question about the current state";
//   * "Climate SHALL feed BIOME POTENTIAL (see `environment-fields`), which procedural generation
//     consumes."
//
// ================================================================================================
// HOW "CLIMATE IS NOT RECOMPUTED" IS CHECKED RATHER THAN PROMISED
// ================================================================================================
//
// `ClimateMap` counts its own evaluations. `evaluations()` is the number of times a climate value
// was actually derived, and `test_climate.cpp` drives ten thousand environment samples through a
// live `WeatherSystem` and requires that number not to move. Climate reaches the current state at
// exactly one place — `WeatherCells::rebase()`, when a cell is (re)seeded — and an environment
// sample reads a weather cell, never a climate one.
//
// That is also why `sample()` is `const` and cheap-but-not-free: a climate map may be an authored
// grid (a designer painted it) or a derived model (latitude, elevation, distance to ocean), and the
// derived one costs real arithmetic. A subsystem that sampled it per query would be paying that
// arithmetic per query, which is the failure the requirement names.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/world/coordinates.h>

namespace cy::weather {

/// The prevailing tendencies at a place. Every member is one of the six the specification's table
/// names, in the unit a designer would author it in.
struct ClimateSample {
    /// Annual mean, degrees Celsius.
    f32 mean_temperature_celsius = 14.0F;
    /// Annual swing about the mean, degrees Celsius. Continental interiors swing; coasts do not,
    /// which is what `ocean_influence` below is derived into.
    f32 temperature_range_celsius = 12.0F;
    /// Annual mean relative humidity, [0, 1].
    f32 humidity = 0.6F;
    /// The prevailing wind, m/s, world axes, x in `.x` and z in `.y`. Horizontal: a prevailing
    /// VERTICAL wind is not a climate, it is a mountain, and it belongs to terrain influence.
    Vec2 prevailing_wind{4.0F, 0.0F};
    /// Millimetres per year. `potential` and not `rate`: it is what the place tends to receive, and
    /// the current rate is weather's.
    f32 rainfall_potential_mm = 700.0F;
    /// Fraction of the year's daylight that reaches the ground, [0, 1]. Cloudiness and terrain
    /// shading folded together, because a consumer of it — evaporation, snow melt, vegetation —
    /// cannot tell the two apart.
    f32 solar_exposure = 0.6F;
    /// [0, 1]: 1 at the coast, 0 deep inland. Moderates temperature range and raises humidity.
    f32 ocean_influence = 0.3F;
};

/// What climate feeds procedural generation. `environment-fields` names `biome` as a standard
/// field, and `weather-and-wind` says climate feeds biome POTENTIAL — the biome a place tends
/// toward, which is not the biome it currently is. Ecosystem state evolves toward this; a fire
/// moves the current state and leaves the potential where it was.
struct BiomePotential {
    /// Vegetation density the place supports, [0, 1].
    f32 vegetation = 0.5F;
    /// Soil moisture the place tends to, [0, 1].
    f32 moisture = 0.5F;
    /// The biome index this climate tends toward, through `classify_biome()`.
    u32 biome = 0;
};

/// The biomes the engine names. A project declares more by using indices beyond `kProjectBiome`;
/// nothing here switches on the value except `classify_biome()`, which a project replaces by
/// declaring its own `BiomeRule` set (see ecosystem.h).
namespace biomes {
inline constexpr u32 kBarren = 0;
inline constexpr u32 kDesert = 1;
inline constexpr u32 kGrassland = 2;
inline constexpr u32 kSavanna = 3;
inline constexpr u32 kShrubland = 4;
inline constexpr u32 kForest = 5;
inline constexpr u32 kRainforest = 6;
inline constexpr u32 kTaiga = 7;
inline constexpr u32 kTundra = 8;
inline constexpr u32 kProjectBiome = 64;
}  // namespace biomes

[[nodiscard]] const char* biome_name(u32 biome) noexcept;

/// The engine's default classification of a climate into a biome. Temperature against moisture,
/// which is Whittaker's axes and is the shape every biome diagram in the literature has.
///
/// It is a FUNCTION OF THE CLIMATE and not of the current state, deliberately: the current biome is
/// the ecosystem's, decided by declared thresholds over fields (ecosystem.h), and a place whose
/// vegetation has burned away is still forest country.
[[nodiscard]] BiomePotential climate_biome_potential(const ClimateSample& climate) noexcept;

/// How a climate map answers. Both are authored data by the specification's own words — "mostly
/// authored or generated" — and the difference is only who wrote the grid.
enum class ClimateSourceKind : u8 {
    /// A uniform climate. What a small level and every test that is not about climate wants.
    Uniform = 0,
    /// A painted grid of samples, bilinearly interpolated.
    Grid,
    /// Derived from latitude, elevation and distance to the ocean, by `derive_climate()`.
    Derived,
};

/// Inputs to the derived model. A generator's parameters, not a simulation's: this is run once per
/// climate cell when a map is built, and never again.
struct ClimateModel {
    /// Metres of world per degree of latitude. A planet-scale world sets it from its radius; a
    /// hundred-kilometre world sets it large so the whole map is one latitude band.
    f64 metres_per_degree = 111'000.0;
    /// The latitude at world z = 0, degrees.
    f64 latitude_at_origin = 45.0;
    /// Degrees Celsius lost per kilometre of elevation. The environmental lapse rate.
    f32 lapse_rate_per_km = 6.5F;
    /// Metres beyond which the ocean no longer moderates the climate.
    f32 ocean_reach_metres = 120'000.0F;
    /// Mean sea-level temperature at the equator and at the pole, which the latitude band
    /// interpolates between.
    f32 equator_temperature_celsius = 27.0F;
    f32 pole_temperature_celsius = -20.0F;
    /// The prevailing wind at the reference latitude, m/s.
    Vec2 prevailing_wind{6.0F, 1.0F};
};

/// Ground elevation and distance to the ocean at a horizontal position, in metres.
///
/// THE TERRAIN AND WATER SEAM, as two callbacks. Weather does not link `cy::terrain` or
/// `cy::water` — see this module's CMakeLists — and what it needs from both is a number. A world
/// with neither passes nothing and gets a flat, inland climate, which is the correct answer for a
/// world with no mountains and no sea.
struct ClimateTerrain {
    f64 (*elevation_at)(void* user, f64 x, f64 z) noexcept = nullptr;
    f64 (*ocean_distance_at)(void* user, f64 x, f64 z) noexcept = nullptr;
    void* user = nullptr;
};

/// One derived climate sample. Free, so a cooker deriving a climate grid offline runs the same
/// arithmetic the runtime would.
[[nodiscard]] ClimateSample derive_climate(const ClimateModel& model, const ClimateTerrain& terrain,
                                           f64 x, f64 z) noexcept;

/// The climate of a world: a coarse grid, sampled bilinearly, that nothing reads per frame.
///
/// The grid is COARSE by construction — tens of kilometres per cell — because climate is the layer
/// that changes rarely and a fine climate grid is a weather grid that has forgotten which layer it
/// is on.
class ClimateMap {
public:
    explicit ClimateMap(Allocator& allocator) noexcept;

    ClimateMap(const ClimateMap&) = delete;
    ClimateMap& operator=(const ClimateMap&) = delete;

    /// A world with one climate everywhere.
    [[nodiscard]] Status set_uniform(const ClimateSample& climate) noexcept;

    /// An authored grid. `origin` is the world position of cell (0, 0)'s CENTRE; the grid covers
    /// `width` by `height` cells of `cell_metres` and saturates at its edges, so a position outside
    /// it gets the nearest cell rather than a default nobody authored.
    [[nodiscard]] Status set_grid(f64 origin_x, f64 origin_z, f32 cell_metres, u32 width,
                                  u32 height, Span<const ClimateSample> cells) noexcept;

    /// Build the grid from `derive_climate()`, once. Every cell is evaluated here and never again,
    /// which is what makes the derived model affordable: a hundred-kilometre world at 20 km cells
    /// is 25 evaluations.
    [[nodiscard]] Status derive(const ClimateModel& model, const ClimateTerrain& terrain,
                                f64 origin_x, f64 origin_z, f32 cell_metres, u32 width,
                                u32 height) noexcept;

    [[nodiscard]] ClimateSample sample(f64 x, f64 z) const noexcept;
    [[nodiscard]] ClimateSample sample(const world::WorldVec3d& at) const noexcept;
    [[nodiscard]] BiomePotential potential(f64 x, f64 z) const noexcept;

    [[nodiscard]] ClimateSourceKind kind() const noexcept { return kind_; }
    [[nodiscard]] u32 width() const noexcept { return width_; }
    [[nodiscard]] u32 height() const noexcept { return height_; }
    [[nodiscard]] f32 cell_metres() const noexcept { return cell_metres_; }

    /// Climate evaluations since construction. See the header note: this is the number a suite
    /// requires not to move while the current state is being queried, and it is the whole of the
    /// "climate SHALL NOT be recomputed" enforcement.
    [[nodiscard]] u64 evaluations() const noexcept { return evaluations_; }
    void reset_evaluations() const noexcept { evaluations_ = 0; }

private:
    [[nodiscard]] ClimateSample fetch(i64 i, i64 k) const noexcept;

    Allocator* allocator_;
    Array<ClimateSample> cells_;
    ClimateSample uniform_;
    ClimateSourceKind kind_ = ClimateSourceKind::Uniform;
    f64 origin_x_ = 0.0;
    f64 origin_z_ = 0.0;
    f32 cell_metres_ = 20'000.0F;
    u32 width_ = 0;
    u32 height_ = 0;
    /// Mutable because counting an evaluation is not a change to what the map answers, and a
    /// diagnostic that forced every sampler to hold a non-const map would be one nobody leaves on.
    /// `environment::FieldStore` counts its sample cost the same way, for the same reason.
    mutable u64 evaluations_ = 0;
};

}  // namespace cy::weather
