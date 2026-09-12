#pragma once
// The worlds CyberWeather's suites are written against.
//
// One climate, one mountain range across the prevailing wind, and one grid that deliberately does
// not line up with a world partition — because those three are what the specification's own
// scenarios need, and because a fixture with a flat world and an aligned grid would let a suite
// pass while the rain shadow and the non-alignment were both broken.

#include <cy/core/memory/system_allocator.h>
#include <cy/weather/system.h>

namespace cy::weather::test {

[[nodiscard]] inline Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// A partition with 128 m level-0 cells at the origin — the shape src/world/'s, src/environment/'s
/// and src/water/'s own fixtures use, so a cell footprint computed here is the one those modules
/// would compute.
///
/// Returned BY REFERENCE to a function-local constant, not by value: `environment::FieldStore`
/// holds the configuration by reference, so a fixture returning a temporary would hand every suite
/// a dangling reference the moment the constructor returned. src/water/tests/fixtures.h records the
/// afternoon that cost; it is recorded again here so it costs the next reader nothing.
[[nodiscard]] inline const world::PartitionConfig& partition() noexcept {
    static const world::PartitionConfig config = [] {
        world::PartitionConfig value;
        value.partition = 1;
        value.base_cell_size = 128.0F;
        value.levels = 3;
        value.level_ratio = 4;
        return value;
    }();
    return config;
}

/// A cold climate, for the suites about snow. A snowstorm preset over a 14 C climate produces a
/// cell at about 3 C — because a regional target is half the PLACE's own climate and half the
/// world's mood, which is the hierarchy doing exactly what it is for. Snow needs a cold place, not
/// a colder preset.
[[nodiscard]] inline ClimateSample cold_climate() noexcept;

/// A temperate climate with a westerly prevailing wind. Uniform, so that a suite measuring the rain
/// shadow measures the TERRAIN's effect and not the climate map's gradient.
[[nodiscard]] inline ClimateSample temperate_climate() noexcept {
    ClimateSample climate;
    climate.mean_temperature_celsius = 14.0F;
    climate.temperature_range_celsius = 12.0F;
    climate.humidity = 0.75F;
    climate.prevailing_wind = Vec2{10.0F, 0.0F};  // due east, along +x
    climate.rainfall_potential_mm = 900.0F;
    climate.solar_exposure = 0.55F;
    climate.ocean_influence = 0.5F;
    return climate;
}

/// A ridge running north-south across the prevailing wind, crested at x = 20 km.
///
/// The shape matters: a smooth 2 km-wide ridge 1 200 m high, so the windward slope is a real slope
/// over several weather cells rather than a cliff one cell wide that the grid cannot see.
[[nodiscard]] inline ClimateSample cold_climate() noexcept {
    ClimateSample climate;
    climate.mean_temperature_celsius = -6.0F;
    climate.temperature_range_celsius = 18.0F;
    climate.humidity = 0.7F;
    climate.prevailing_wind = Vec2{8.0F, 0.0F};
    climate.rainfall_potential_mm = 500.0F;
    climate.solar_exposure = 0.4F;
    climate.ocean_influence = 0.3F;
    return climate;
}

[[nodiscard]] inline f64 ridge_elevation(void*, f64 x, f64) noexcept {
    const f64 offset = (x - 20'000.0) / 6'000.0;
    return 1'200.0 / (1.0 + (offset * offset));
}

[[nodiscard]] inline TerrainProfile ridge() noexcept {
    TerrainProfile profile;
    profile.elevation_at = ridge_elevation;
    return profile;
}

/// The grid every suite uses: 4 km cells, an origin at a prime number of metres, and 24 x 8 cells.
///
/// **IT DOES NOT ALIGN WITH `partition()`, ON PURPOSE.** 4 000 m is not a multiple of 128 m and the
/// origin is not the partition's, so every weather cell boundary falls inside a world cell and
/// every world cell boundary falls inside a weather cell. `test_cells.cpp` asserts `aligns_with()`
/// is false and then runs the whole model on it — which is how "weather cells SHALL NOT be required
/// to align with world partition cells" is measured rather than commented.
[[nodiscard]] inline WeatherGridConfig grid() noexcept {
    WeatherGridConfig config;
    config.origin_x = -511.0;
    config.origin_z = -257.0;
    config.regional_cell_metres = 4'000.0F;
    config.width = 24;
    config.height = 8;
    config.refine_ratio = 8;
    config.max_local_cells = 512;
    config.macro_step_seconds = 60.0F;
    config.relaxation_per_second = 0.0015F;
    return config;
}

/// A whole configuration, with small field resolutions so that a publication over a test region is
/// a few hundred lattice points rather than a few hundred thousand.
[[nodiscard]] inline WeatherConfig config() noexcept {
    WeatherConfig value;
    value.grid = grid();
    value.orographic = OrographicModel{};
    value.gust = GustModel{};
    value.turbulence = TurbulenceModel{};
    value.fields.local_cell_metres = 64.0F;
    value.fields.regional_cell_metres = 256.0F;
    value.fields.macro_cell_metres = 1'024.0F;
    value.fields.wind_vertical_cells = 2;
    value.fields.wind_vertical_metres = 32.0F;
    value.seed = 0x4321'5678'9ABC'DEF0ULL;
    // ONE TICK IS ONE MINUTE OF SIMULATED TIME IN THESE SUITES, and that is what this rate says.
    // A suite that advanced a minute of weather per call while the director believed a tick was a
    // sixtieth of a second would have transitions running sixty times slower than the cells they
    // drive — which is exactly the mismatch that made the first version of `test_fields.cpp` report
    // a storm preset applied and no rain. The rate is a declaration, so declaring it honestly here
    // is the fix rather than a workaround.
    value.ticks_per_second = 1.0 / 60.0;

    // THE ECOSYSTEM'S RATES ARE ACCELERATED, AND THAT IS DELIBERATE. A shipping world's vegetation
    // closes a ten-millionth of its gap per second, so measuring "a burned forest regrows" at that
    // rate costs a suite thousands of simulated years and seconds of real time for a mechanism that
    // is the same at any rate. These are the same fields, the same substrate recovery and the same
    // step; only the constant differs, and `Ecosystem::check_resolution()` still has to pass on it.
    value.fields.vegetation_recovery_per_second = 2.0e-6F;
    value.fields.moisture_recovery_per_second = 2.0e-5F;
    return value;
}

/// A publication region of a few macro cells — three by three at the default macro resolution.
[[nodiscard]] inline PublishRegion small_region() noexcept {
    PublishRegion region;
    region.min_x = 0.0;
    region.min_z = 0.0;
    region.max_x = 2'048.0;
    region.max_z = 2'048.0;
    region.level = environment::FieldResidency::Macro;
    return region;
}

/// A storm at a position, of a type, with a lifetime long enough not to expire mid-test.
[[nodiscard]] inline Storm storm_at(u64 id, f64 x, f64 z, StormType type, f32 intensity) noexcept {
    Storm storm;
    storm.id = StormId{id};
    storm.type = type;
    storm.position = world::WorldVec3d{x, 0.0, z};
    storm.velocity = Vec2{8.0F, 0.0F};
    storm.radius_metres = 12'000.0F;
    storm.intensity = intensity;
    storm.seed = 0xABCD'0000ULL + id;
    storm.lifetime_seconds = 0.0F;
    return storm;
}

[[nodiscard]] inline determinism::SimulationPoint at_tick(u64 tick) noexcept {
    determinism::SimulationPoint point;
    point.tick = tick;
    return point;
}

}  // namespace cy::weather::test
