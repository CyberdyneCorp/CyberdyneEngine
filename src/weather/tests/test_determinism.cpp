// Weather's determinism declaration, measured: presentation levers move nothing authoritative, two
// systems from one seed agree bit for bit, and a replicated snapshot reproduces the sender.
// M10 task 3.2, and `weather-and-wind`'s "Weather determinism and the firewall" and "Weather in
// replay and networking".
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL — reported in `verified_failing`: `WeatherSystem::
// precipitation_plan()` was changed to write its computed particle count back into the weather cell
// it sampled (via a `const_cast`, which is what such a bug looks like in practice). "a quality tier
// changes no authoritative number" went red on the first tick, reporting a rain rate that differed
// between the two systems by the ratio of their particle budgets. The write was removed.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/environment/field.h>
#include <cy/environment/store.h>
#include <cy/weather/system.h>

#include <cmath>

#include "fixtures.h"

namespace test = cy::weather::test;
using cy::environment::FieldRegistry;
using cy::environment::FieldStore;
using cy::weather::ClimateMap;
using cy::weather::EnvironmentSample;
using cy::weather::PrecipitationLevers;
using cy::weather::SampleQuality;
using cy::weather::StormType;
using cy::weather::WeatherConfig;
using cy::weather::WeatherSystem;

namespace {

[[nodiscard]] ClimateMap& uniform_climate() {
    static ClimateMap map(test::allocator());
    static const bool ready = [] {
        return map.set_uniform(test::temperate_climate()).has_value();
    }();
    (void)ready;
    return map;
}

/// A world with everything moving: a storm crossing it, a ridge under it, and a preset arriving.
[[nodiscard]] cy::Status arm(WeatherSystem& system) noexcept {
    system.set_terrain(test::ridge());
    if (cy::Status spawned =
            system.storms().spawn(test::storm_at(1, 0.0, 0.0, StormType::Thunderstorm, 0.85F));
        !spawned) {
        return spawned;
    }
    return system.director().apply(cy::weather::preset_storm(),
                                   cy::weather::TransitionProfile::standard(), 0);
}

/// The authoritative surface of a sample: everything a gameplay system can see. Deliberately NOT
/// the turbulence, which is what the presentation levers are allowed to move.
void check_identical(const EnvironmentSample& a, const EnvironmentSample& b) noexcept {
    CY_CHECK_EQ(a.temperature_celsius, b.temperature_celsius);
    CY_CHECK_EQ(a.humidity, b.humidity);
    CY_CHECK_EQ(a.pressure_hpa, b.pressure_hpa);
    CY_CHECK_EQ(a.precipitation_mm_per_hour, b.precipitation_mm_per_hour);
    CY_CHECK(a.precipitation_type == b.precipitation_type);
    CY_CHECK_EQ(a.cloud_coverage, b.cloud_coverage);
    CY_CHECK_EQ(a.visibility_metres, b.visibility_metres);
    CY_CHECK_EQ(a.wetness_potential, b.wetness_potential);
    CY_CHECK_EQ(a.wind.authoritative().x, b.wind.authoritative().x);
    CY_CHECK_EQ(a.wind.authoritative().y, b.wind.authoritative().y);
    CY_CHECK_EQ(a.wind.authoritative().z, b.wind.authoritative().z);
}

}  // namespace

CY_TEST_CASE("a quality tier changes no authoritative number, over a thousand ticks") {
    // "WHEN a client renders heavier rain at a higher quality tier THEN gameplay outcomes SHALL be
    // unchanged", and "WHEN GPU budget pressure reduces cloud and volumetric quality THEN wind,
    // rain intensity, and visibility values SHALL be unchanged."
    //
    // Two systems from one seed, ticked in lockstep, one at the lowest presentation levers and one
    // at the highest. A thousand ticks, because a difference that appears on tick one is a bug
    // anybody would find and a difference that appears on tick nine hundred is the one that ships.
    WeatherSystem rich(test::allocator());
    WeatherSystem poor(test::allocator());
    CY_REQUIRE(rich.configure(test::config(), uniform_climate()).has_value());
    CY_REQUIRE(poor.configure(test::config(), uniform_climate()).has_value());
    CY_REQUIRE(arm(rich).has_value());
    CY_REQUIRE(arm(poor).has_value());

    PrecipitationLevers cinematic;
    cinematic.max_particles = 65'536;
    cinematic.density_scale = 1.0F;
    cinematic.particle_distance_metres = 200.0F;
    PrecipitationLevers minimum;
    minimum.max_particles = 0;
    minimum.density_scale = 0.0F;
    minimum.particle_distance_metres = 1.0F;
    rich.set_presentation(cinematic);
    poor.set_presentation(minimum);

    const cy::world::WorldVec3d probes[4] = {{0.0, 2.0, 0.0},
                                             {14'000.0, 2.0, 500.0},
                                             {26'000.0, 40.0, -1'200.0},
                                             {61'000.0, 2.0, 3'000.0}};
    for (cy::u32 tick = 1; tick <= 1'000; ++tick) {
        CY_REQUIRE(rich.advance(test::at_tick(tick), 30.0).has_value());
        CY_REQUIRE(poor.advance(test::at_tick(tick), 30.0).has_value());
        if (tick % 173 != 0) {
            continue;
        }
        for (const cy::world::WorldVec3d& at : probes) {
            check_identical(rich.sample(at, SampleQuality::Gameplay,
                                        cy::determinism::SimulationClass::Authoritative),
                            poor.sample(at, SampleQuality::Gameplay,
                                        cy::determinism::SimulationClass::Authoritative));
        }
    }

    // And the levers DID move the presentation, or the equality above would be true of a system
    // that ignores them.
    CY_CHECK_GT(rich.precipitation_plan(probes[0]).particles[0],
                poor.precipitation_plan(probes[0]).particles[0]);
    CY_CHECK_EQ(poor.precipitation_plan(probes[0]).particles[0], 0u);
    // The rate and the type are identical in the plan too: they are state, carried through so a
    // diagnostic does not have to re-derive them, and a tier does not touch them.
    CY_CHECK_EQ(rich.precipitation_plan(probes[0]).rate_mm_per_hour,
                poor.precipitation_plan(probes[0]).rate_mm_per_hour);
}

CY_TEST_CASE("two systems from one seed evolve identically") {
    // "Authoritative weather evolution SHALL be reproducible from its seed and recorded events."
    WeatherSystem a(test::allocator());
    WeatherSystem b(test::allocator());
    CY_REQUIRE(a.configure(test::config(), uniform_climate()).has_value());
    CY_REQUIRE(b.configure(test::config(), uniform_climate()).has_value());
    CY_REQUIRE(arm(a).has_value());
    CY_REQUIRE(arm(b).has_value());

    for (cy::u32 tick = 1; tick <= 600; ++tick) {
        CY_REQUIRE(a.advance(test::at_tick(tick), 20.0).has_value());
        CY_REQUIRE(b.advance(test::at_tick(tick), 20.0).has_value());
    }
    const cy::world::WorldVec3d at{9'000.0, 6.0, 400.0};
    check_identical(
        a.sample(at, SampleQuality::Gameplay, cy::determinism::SimulationClass::Authoritative),
        b.sample(at, SampleQuality::Gameplay, cy::determinism::SimulationClass::Authoritative));
    CY_CHECK_EQ(a.storms().size(), b.storms().size());

    // A DIFFERENT SEED is a different world, or the equality above would be true of a model with no
    // randomness in it at all.
    WeatherConfig other = test::config();
    other.seed ^= 0xFFFFULL;
    WeatherSystem c(test::allocator());
    CY_REQUIRE(c.configure(other, uniform_climate()).has_value());
    CY_REQUIRE(arm(c).has_value());
    for (cy::u32 tick = 1; tick <= 600; ++tick) {
        CY_REQUIRE(c.advance(test::at_tick(tick), 20.0).has_value());
    }
    const cy::weather::WindSample one =
        a.sample(at, SampleQuality::Gameplay, cy::determinism::SimulationClass::Authoritative).wind;
    const cy::weather::WindSample two =
        c.sample(at, SampleQuality::Gameplay, cy::determinism::SimulationClass::Authoritative).wind;
    CY_CHECK_NE(one.gust.x, two.gust.x);
}

CY_TEST_CASE("a snapshot carries the storms and the grid, and nothing that is drawn") {
    // "Networking SHALL replicate LOW-FREQUENCY AUTHORITATIVE STATE: storm identity, position,
    // velocity, intensity, regional weather state, and events such as lightning. Clients SHALL
    // reconstruct visual detail locally." And "A replay SHALL NEVER need to record cloud volumes,
    // particle state, or rendered output."
    WeatherSystem server(test::allocator());
    CY_REQUIRE(server.configure(test::config(), uniform_climate()).has_value());
    CY_REQUIRE(arm(server).has_value());
    for (cy::u32 tick = 1; tick <= 400; ++tick) {
        CY_REQUIRE(server.advance(test::at_tick(tick), 30.0).has_value());
    }

    cy::Array<cy::u8> message(test::allocator());
    CY_REQUIRE(server.encode_state(message).has_value());
    CY_CHECK_EQ(message.size(), server.encoded_state_bytes());
    // THE SIZE CLAIM: the grid is 24 x 8 cells of 37 bytes, plus a storm of 65 bytes and two
    // headers. A world-scale cloud field would be orders of magnitude more, and there is nothing in
    // the module that could put one here.
    CY_CHECK_LT(message.size(), 8'192u);

    WeatherSystem client(test::allocator());
    CY_REQUIRE(client.configure(test::config(), uniform_climate()).has_value());
    CY_REQUIRE(client.decode_state(message.span()).has_value());

    // The client reconstructs the same weather from the snapshot, without having simulated it.
    CY_REQUIRE_EQ(client.storms().size(), server.storms().size());
    for (const cy::world::WorldVec3d at :
         {cy::world::WorldVec3d{0.0, 0.0, 0.0}, cy::world::WorldVec3d{20'000.0, 0.0, 1'000.0}}) {
        const EnvironmentSample sent = server.sample(
            at, SampleQuality::Macro, cy::determinism::SimulationClass::Authoritative);
        const EnvironmentSample got = client.sample(
            at, SampleQuality::Macro, cy::determinism::SimulationClass::Authoritative);
        CY_CHECK_EQ(sent.temperature_celsius, got.temperature_celsius);
        CY_CHECK_EQ(sent.precipitation_mm_per_hour, got.precipitation_mm_per_hour);
        CY_CHECK_EQ(sent.visibility_metres, got.visibility_metres);
        CY_CHECK_EQ(sent.pressure_hpa, got.pressure_hpa);
    }

    // A snapshot carries STATE, not shape: one encoded for a different grid is refused rather than
    // resizing the receiver, which is what keeps its size proportional to the world.
    WeatherConfig wider = test::config();
    wider.grid.width = 40;
    WeatherSystem mismatched(test::allocator());
    CY_REQUIRE(mismatched.configure(wider, uniform_climate()).has_value());
    CY_CHECK_FALSE(mismatched.decode_state(message.span()).has_value());
    // And a truncated one is refused rather than read past its end.
    CY_CHECK_FALSE(client.decode_state(cy::Span<const cy::u8>(message.begin(), 12)).has_value());
}

CY_TEST_CASE("the cloud drive is authoritative state and carries no quality tier") {
    // `atmosphere-sky-and-clouds` declares `CloudWeatherState` and says weather produces it. The
    // producer's half is `CloudDrive`, and the requirement it has to meet from this side is that a
    // quality tier cannot reach it — which is enforced by it not being in the signature.
    WeatherSystem rich(test::allocator());
    WeatherSystem poor(test::allocator());
    CY_REQUIRE(rich.configure(test::config(), uniform_climate()).has_value());
    CY_REQUIRE(poor.configure(test::config(), uniform_climate()).has_value());
    CY_REQUIRE(arm(rich).has_value());
    CY_REQUIRE(arm(poor).has_value());
    PrecipitationLevers minimum;
    minimum.max_particles = 0;
    poor.set_presentation(minimum);

    for (cy::u32 tick = 1; tick <= 200; ++tick) {
        CY_REQUIRE(rich.advance(test::at_tick(tick), 30.0).has_value());
        CY_REQUIRE(poor.advance(test::at_tick(tick), 30.0).has_value());
    }
    // Under the storm, wherever it has drifted to by now: `arm()` gives it a velocity, and probing
    // a fixed point would measure how far it had travelled rather than what it contributes.
    CY_REQUIRE((rich.storms().size()) > (0u));
    const cy::world::WorldVec3d at = rich.storms().storms()[0].position;
    const cy::weather::CloudDrive a = rich.cloud_drive(at);
    const cy::weather::CloudDrive b = poor.cloud_drive(at);
    CY_CHECK_EQ(a.humidity, b.humidity);
    CY_CHECK_EQ(a.precipitation_mm_per_hour, b.precipitation_mm_per_hour);
    CY_CHECK_EQ(a.temperature_celsius, b.temperature_celsius);
    CY_CHECK_EQ(a.wind.x, b.wind.x);
    CY_CHECK_EQ(a.storm_intensity, b.storm_intensity);
    // The storm is over the probe, so the drive actually carries something.
    CY_CHECK_GT(a.storm_intensity, 0.0F);
    CY_CHECK_GT(a.epoch, 0u);
}

CY_TEST_CASE("the whole tick is reproducible against a live field store") {
    // The same claim as above, but with the fields bound, because publication reads the sampler and
    // a publication that depended on presentation would put presentation into persistent state.
    FieldRegistry left_registry(test::allocator());
    FieldStore left_store(test::allocator(), left_registry, test::partition());
    FieldRegistry right_registry(test::allocator());
    FieldStore right_store(test::allocator(), right_registry, test::partition());

    WeatherSystem left(test::allocator());
    WeatherSystem right(test::allocator());
    CY_REQUIRE(left.configure(test::config(), uniform_climate()).has_value());
    CY_REQUIRE(right.configure(test::config(), uniform_climate()).has_value());
    CY_REQUIRE(left.bind_fields(left_registry, left_store).has_value());
    CY_REQUIRE(right.bind_fields(right_registry, right_store).has_value());
    left.set_publish_region(test::small_region());
    right.set_publish_region(test::small_region());
    CY_REQUIRE(
        left.director()
            .apply(cy::weather::preset_storm(), cy::weather::TransitionProfile::uniform(120.0F), 0)
            .has_value());
    CY_REQUIRE(
        right.director()
            .apply(cy::weather::preset_storm(), cy::weather::TransitionProfile::uniform(120.0F), 0)
            .has_value());
    PrecipitationLevers minimum;
    minimum.max_particles = 0;
    right.set_presentation(minimum);

    for (cy::u32 tick = 1; tick <= 120; ++tick) {
        CY_REQUIRE(left.advance(test::at_tick(tick), 60.0).has_value());
        CY_REQUIRE(right.advance(test::at_tick(tick), 60.0).has_value());
    }

    const cy::world::WorldVec3d at{1'024.0, 0.0, 1'024.0};
    const cy::weather::WeatherField published[5] = {
        cy::weather::WeatherField::Wind, cy::weather::WeatherField::Temperature,
        cy::weather::WeatherField::PrecipitationRate, cy::weather::WeatherField::Wetness,
        cy::weather::WeatherField::SnowDepth};
    for (cy::weather::WeatherField field : published) {
        const cy::f32 a = left_store.sample_deterministic(left.fields().id(field), at).value.x();
        const cy::f32 b = right_store.sample_deterministic(right.fields().id(field), at).value.x();
        CY_CHECK_EQ(a, b);
    }
}
