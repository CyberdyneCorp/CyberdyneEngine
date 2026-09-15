// THE CLOUD SHADOW FIELD, THROUGH CyberField AND NOT BESIDE IT.
//
// Integration, because every case here drives a real `environment::FieldRegistry` and a real
// `environment::FieldStore` and writes real tiles — and because writing a shadow cell is a ray
// march through the cloud reconstruction, which is what makes the field coarse in the first place.

#include <cy/test/test.h>

#include <cy/core/math/math.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/environment/field.h>
#include <cy/environment/store.h>
#include <cy/rendering/sky/cloud_shadows.h>
#include <cy/rendering/sky/clouds.h>
#include <cy/world/coordinates.h>

#include <cmath>
#include <cstring>

namespace {

using cy::f32;
using cy::u32;
using cy::Vec3;
using cy::world::WorldVec3d;
using namespace cy::rendering::sky;

[[nodiscard]] cy::Allocator& allocator() {
    return cy::system_allocator(cy::MemoryDomain::World);
}

[[nodiscard]] cy::world::PartitionConfig partition() {
    cy::world::PartitionConfig config;
    config.partition = 1;
    config.base_cell_size = 128.0F;
    config.levels = 3;
    config.level_ratio = 4;
    return config;
}

/// A shadow-caster small enough for an integration budget: a 512 m radius at 128 m cells is a
/// handful of tiles, and the property under test is what the field CONTAINS rather than how much of
/// the world it covers.
[[nodiscard]] CloudShadowQuality test_quality() {
    CloudShadowQuality quality;
    quality.regional_cell_metres = 128.0F;
    quality.macro_cell_metres = 1024.0F;
    quality.radius_metres = 512.0F;
    quality.updates_per_second = 8.0F;
    quality.steps = 8;
    return quality;
}

/// A sky thick enough to cast a shadow somebody can measure.
struct Clouds {
    CloudWeatherMap map;
    CloudField field;

    [[nodiscard]] cy::Status build(f32 coverage, f32 storminess) {
        if (auto status = map.configure(16, 1000.0F); !status) {
            return status;
        }
        if (auto status = map.generate(0x5ADE5ULL, coverage, storminess); !status) {
            return status;
        }
        field.map = &map;
        field.layers = default_cloud_layers();
        CloudWeatherState weather;
        weather.humidity = coverage;
        weather.storm_intensity = storminess;
        drive_cloud_layers(weather, field.layers);
        return {};
    }
};

}  // namespace

CY_TEST_CASE("cloud shadows: the field is claimed, and a second producer is refused naming both") {
    cy::environment::FieldRegistry registry(allocator());
    CloudShadowField sky;
    CY_REQUIRE(sky.attach(registry, test_quality()));
    CY_CHECK(sky.attached());

    // "Two systems writing one field SHALL be a configuration error detected at startup or cook
    // time, not a last-writer-wins race resolved at runtime." A second sky over one registry is
    // exactly that mistake, and it is refused rather than merging.
    // A DIFFERENT producer name, because the refusal's job is to name the two of them apart and a
    // test where both are called the same thing cannot tell whether it did.
    CloudShadowField second;
    const auto status = second.attach(registry, test_quality(), "project.custom-sky");
    CY_CHECK_FALSE(status);
    CY_CHECK_FALSE(second.attached());

    // And the refusal NAMES BOTH: the incumbent and the challenger, in the structured conflict the
    // substrate records. A diagnostic that said only "already claimed" would leave a project
    // hunting for which of its systems got there first.
    const cy::environment::ProducerConflict& conflict = registry.last_conflict();
    CY_CHECK(conflict.occurred());
    CY_CHECK_EQ(conflict.field, cloud_shadow_field_id());
    CY_CHECK(std::strcmp(conflict.incumbent, kCloudShadowProducerName) == 0);
    CY_CHECK(std::strcmp(conflict.challenger, "project.custom-sky") == 0);
    // The message itself is the substrate's, and it names both producers — it is printed by the
    // engine's log at the moment of the refusal rather than here, because doctest renders a
    // `const char*` as its address.
}

CY_TEST_CASE("cloud shadows: the four consumers may read it and gameplay may not") {
    cy::environment::FieldRegistry registry(allocator());
    CloudShadowField sky;
    CY_REQUIRE(sky.attach(registry, test_quality()));
    CY_REQUIRE(CloudShadowField::declare_consumers(registry));

    cy::Array<cy::environment::FirewallViolation> violations(allocator());
    CY_REQUIRE(registry.validate(violations));
    // Terrain materials, foliage, water shading and illumination: the four the requirement names,
    // each reading as presentation, which is what the field is.
    CY_CHECK(violations.empty());

    // A gameplay system reading a presentation field is caught by CONFIGURATION VALIDATION, before
    // a frame has run — which is `environment-fields`' own mechanism and not a second one. The
    // cloud shadow is derived from the frame's time and is allowed to change with a quality tier;
    // a simulation that branched on it would be a simulation two machines could disagree about.
    CY_REQUIRE(registry.declare_consumer("gameplay.stealth",
                                         cy::determinism::SimulationClass::Authoritative,
                                         cloud_shadow_field_id()));
    violations.clear();
    CY_REQUIRE(registry.validate(violations));
    CY_REQUIRE_EQ(violations.size(), 1U);
    CY_CHECK(std::strcmp(violations[0].consumer, "gameplay.stealth") == 0);
}

CY_TEST_CASE("cloud shadows: an update writes the field, and a storm darkens the ground") {
    cy::environment::FieldRegistry registry(allocator());
    CloudShadowField sky;
    CY_REQUIRE(sky.attach(registry, test_quality()));
    cy::environment::FieldStore store(allocator(), registry, partition());

    // BROKEN CLOUD, not overcast. The scenario is "a cloud shadow crosses a valley", and a sky
    // that covers everything produces a field that is uniformly black — which satisfies "the
    // darkest cell is dark" while containing no shadow at all. What a shadow IS is the SPREAD
    // between the lit ground and the shaded ground, so the weather is chosen to have both.
    Clouds broken;
    CY_REQUIRE(broken.build(0.45F, 0.2F));

    const Vec3 sun = normalize(Vec3{0.3F, 0.8F, 0.2F});
    const WorldVec3d centre{0.0, 0.0, 0.0};
    auto wrote = sky.update(store, broken.field, sun, 0.0, centre, 1.0F);
    CY_REQUIRE(wrote);
    CY_CHECK(wrote.value());
    CY_CHECK_GT(sky.stats().tiles_written, 0U);
    CY_CHECK_GT(sky.stats().cells_evaluated, 0ULL);
    CY_TEST_MESSAGE("broken cloud: ", sky.stats().tiles_written, " tiles, ",
                    sky.stats().cells_evaluated, " cells, darkest ", sky.stats().darkest,
                    ", brightest ", sky.stats().brightest, ", mean ", sky.stats().mean);

    // Ground in full sun, ground under a cloud, and a mean between them. Three numbers, because any
    // two of them can be produced by a field that is not a shadow.
    CY_CHECK_LT(sky.stats().darkest, 0.5F);
    CY_CHECK_GT(sky.stats().brightest, 0.9F);
    CY_CHECK_GT(sky.stats().mean, sky.stats().darkest);
    CY_CHECK_LT(sky.stats().mean, sky.stats().brightest);

    // A clear sky over the same ground is brighter. This is the pair that makes the case above mean
    // something: a producer that wrote a constant would pass one of them and not both.
    cy::environment::FieldRegistry clear_registry(allocator());
    CloudShadowField clear_sky;
    CY_REQUIRE(clear_sky.attach(clear_registry, test_quality()));
    cy::environment::FieldStore clear_store(allocator(), clear_registry, partition());
    Clouds clear;
    CY_REQUIRE(clear.build(0.02F, 0.0F));
    CY_REQUIRE(clear_sky.update(clear_store, clear.field, sun, 0.0, centre, 1.0F));
    CY_TEST_MESSAGE("clear: darkest ", clear_sky.stats().darkest, ", mean ",
                    clear_sky.stats().mean);
    CY_CHECK_GT(clear_sky.stats().mean, sky.stats().mean);

    // And it is READ THROUGH THE SUBSTRATE, so terrain, foliage, water and illumination all get the
    // same number rather than each deriving one.
    //
    // ============================================================================================
    // WHAT `m10:sky-field-round-trip` ACTUALLY WAS, WHICH IS NOT WHAT ITS TEXT SAID
    // ============================================================================================
    //
    // The gap said the producer wrote tiles the substrate could not read back: `update()` reported
    // darkest < 0.5 and `sample()` returned the declared 1.0 at all twenty-five points inside
    // `radius_metres`, so it was recorded as a defect in the sky's WRITE PATH. It is not. Walking
    // every cell centre the producer wrote and reading each one back through
    // `FieldStore::sample_at()` resolves 1024 of 1024 regional cells and 256 of 256 macro cells,
    // and the darkest of them is 0.11 at (960, -1600). The store had the shadow all along.
    //
    // What was wrong was THIS CASE'S SAMPLING POSITIONS. It read a five-by-five grid at 128 m
    // spacing about the origin — 256 m in each direction — and that patch of ground is genuinely in
    // full sun under this weather: the cloud map's cells are 1000 m across, the shadow ray lands
    // about 600 m downwind of the sample, and the mean over everything written is 0.974. Twenty-
    // five samples of lit ground read 1.0 whether the field was published or not, which is why
    // suppressing `publish()` did not move them. A round trip measured where nothing was written
    // is the same defect one level up as a producer checked against its own statistics.
    //
    // So the check below reads back EVERY CELL THE PRODUCER WROTE, at both levels, and compares the
    // extremes with what the producer reported. A grid of tiles wider than the radius is walked and
    // unresolved samples are skipped, so this does not re-derive `update()`'s own tile arithmetic
    // and agree with it by construction — the store is asked which cells it holds.
    const auto read_back_level = [&store](cy::environment::FieldResidency level, f32 cell_metres,
                                          f32& lowest, f32& highest) -> u32 {
        u32 resolved = 0;
        const auto span = static_cast<cy::i32>(cy::environment::kTileCells);
        for (cy::i32 tile_z = -2; tile_z <= 1; ++tile_z) {
            for (cy::i32 tile_x = -2; tile_x <= 1; ++tile_x) {
                for (cy::i32 local_z = 0; local_z < span; ++local_z) {
                    for (cy::i32 local_x = 0; local_x < span; ++local_x) {
                        // CELL CENTRES. A linear sample at a cell centre has a fractional
                        // coordinate of exactly zero, so it reads ONE lattice point and the
                        // comparison below is against the stored byte rather than against a blend
                        // of four of them.
                        const double at_x = (static_cast<double>((tile_x * span) + local_x) + 0.5) *
                                            static_cast<double>(cell_metres);
                        const double at_z = (static_cast<double>((tile_z * span) + local_z) + 0.5) *
                                            static_cast<double>(cell_metres);
                        const cy::environment::FieldSample sampled = store.sample_at(
                            cloud_shadow_field_id(), WorldVec3d{at_x, 0.0, at_z}, level);
                        if (!sampled.resolved) {
                            continue;
                        }
                        ++resolved;
                        const f32 value = sampled.value.x();
                        CY_CHECK_GE(value, 0.0F);
                        CY_CHECK_LE(value, 1.0F);
                        lowest = cy::math::min(lowest, value);
                        highest = cy::math::max(highest, value);
                    }
                }
            }
        }
        return resolved;
    };

    f32 lowest = 1.0F;
    f32 highest = 0.0F;
    const u32 regional = read_back_level(cy::environment::FieldResidency::Regional,
                                         test_quality().regional_cell_metres, lowest, highest);
    const u32 macro = read_back_level(cy::environment::FieldResidency::Macro,
                                      test_quality().macro_cell_metres, lowest, highest);
    const u32 sampled_count = regional + macro;
    CY_TEST_MESSAGE("through the store: ", sampled_count, " samples, lowest ", lowest, ", highest ",
                    highest);

    // NOTHING RESOLVED IS THE PUBLISH FAILURE, and it is asserted before the values are, because a
    // field nobody published answers the declared 1.0 at every position and a check that only
    // looked at the numbers could not tell that apart from a cloudless sky.
    CY_CHECK_GT(regional, 0U);
    CY_CHECK_GT(macro, 0U);

    // AND THE FIELD CARRIES THE SHADOW THE PRODUCER SAYS IT COMPUTED. `stats()` is the producer's
    // own account of what it wrote; these two lines are the substrate's account of the same thing,
    // and they are the round trip. A quantum of `UNorm8` is 1/255, which is the whole of the
    // difference the encoding is allowed to introduce — so the tolerance is the encoding's and not
    // a number chosen until the test passed.
    constexpr f32 kQuantum = 1.0F / 255.0F;
    CY_CHECK_NEAR(lowest, sky.stats().darkest, kQuantum);
    CY_CHECK_NEAR(highest, sky.stats().brightest, kQuantum);

    // The two assertions the gap was declared for, now that they are measured where the shadow is.
    // They are what distinguishes a published field from an empty one: the declared default is 1.0,
    // and every other assertion in this case is satisfied by a field nobody ever wrote to.
    CY_CHECK_LT(lowest, 0.99F);
    CY_CHECK_GT(highest - lowest, 0.01F);
    CY_CHECK_GE(highest, lowest);

    // ============================================================================================
    // AND THE SAME CELLS READ BACK THROUGH THE FUNCTION EVERY CONSUMER CALLS. M11.c.
    // ============================================================================================
    //
    // `m10:sky-field-round-trip` was DECLARED against `CloudShadowField::sample` — "sample returned
    // the declared 1.0 at every point the case probed inside radius_metres" — and M11.a's repair
    // rewrote the read-back above to call `store.sample_at(field, position, level)` with an explicit
    // residency instead. That is a better test of the STORE and it stopped being a test of the
    // function the requirement's four consumers call: M11.a's own gate replaced this function's
    // body with `return 1.0F`, rebuilt, and the criterion stayed green, because the only call to it
    // left in this file is the one nine million metres away at the bottom of this case.
    //
    // So the walk is done twice, over the same cell centres, and the two answers are COMPARED. The
    // agreement is the assertion: `CloudShadowField::sample` walks the residencies finest-first and
    // `sample_at` is told which level to read, and at a regional cell centre inside the written
    // radius those two must be the same number. A consumer reading the field through the substrate
    // and a test reading it through a level are then looking at one thing.
    const auto read_back_through_the_consumer = [&store](f32 cell_metres, f32& least, f32& most,
                                                         u32& disagreements) -> u32 {
        u32 resolved = 0;
        const auto span = static_cast<cy::i32>(cy::environment::kTileCells);
        for (cy::i32 tile_z = -2; tile_z <= 1; ++tile_z) {
            for (cy::i32 tile_x = -2; tile_x <= 1; ++tile_x) {
                for (cy::i32 local_z = 0; local_z < span; ++local_z) {
                    for (cy::i32 local_x = 0; local_x < span; ++local_x) {
                        const double at_x = (static_cast<double>((tile_x * span) + local_x) + 0.5) *
                                            static_cast<double>(cell_metres);
                        const double at_z = (static_cast<double>((tile_z * span) + local_z) + 0.5) *
                                            static_cast<double>(cell_metres);
                        const WorldVec3d at{at_x, 0.0, at_z};
                        // The finest resident level is what a consumer gets, so the comparison is
                        // against the FINEST level the store resolves here rather than against a
                        // level this test chose — otherwise a macro cell centre that happens to sit
                        // inside the regional radius would be counted as a disagreement for being
                        // answered correctly.
                        cy::environment::FieldSample finest = store.sample_at(
                            cloud_shadow_field_id(), at, cy::environment::FieldResidency::Regional);
                        if (!finest.resolved) {
                            finest = store.sample_at(cloud_shadow_field_id(), at,
                                                     cy::environment::FieldResidency::Macro);
                        }
                        if (!finest.resolved) {
                            continue;
                        }
                        ++resolved;
                        const f32 through = CloudShadowField::sample(store, at);
                        if (std::fabs(through - finest.value.x()) > 1e-6F) {
                            ++disagreements;
                        }
                        least = cy::math::min(least, through);
                        most = cy::math::max(most, through);
                    }
                }
            }
        }
        return resolved;
    };

    f32 consumer_lowest = 1.0F;
    f32 consumer_highest = 0.0F;
    u32 disagreements = 0;
    const u32 consumer_regional = read_back_through_the_consumer(
        test_quality().regional_cell_metres, consumer_lowest, consumer_highest, disagreements);
    const u32 consumer_macro = read_back_through_the_consumer(
        test_quality().macro_cell_metres, consumer_lowest, consumer_highest, disagreements);
    const u32 consumer_total = consumer_regional + consumer_macro;
    CY_TEST_MESSAGE("through CloudShadowField::sample: ", consumer_total, " samples, lowest ",
                    consumer_lowest, ", highest ", consumer_highest, ", disagreeing ",
                    disagreements);

    CY_CHECK_GT(consumer_regional, 0U);
    CY_CHECK_GT(consumer_macro, 0U);
    // NOT ONE CELL DISAGREES. This is the half `m10:sky-field-round-trip` was declared about and
    // the half no check in this repository has ever performed.
    CY_CHECK_EQ(disagreements, 0U);
    // And the shadow is THERE, in the function a consumer calls, rather than only in the level a
    // test asked for by name. Replacing this function's body with `return 1.0F` — the literal
    // symptom the gap names — makes both of these fail.
    CY_CHECK_LT(consumer_lowest, 0.99F);
    CY_CHECK_GT(consumer_highest - consumer_lowest, 0.01F);

    // A sample far outside the written radius returns the declared default — FULL SUN — rather than
    // blocking or faulting. `environment-fields` requires exactly that, and a default of zero would
    // have blacked out an unstreamed world.
    const f32 distant = CloudShadowField::sample(store, WorldVec3d{9000000.0, 0.0, -9000000.0});
    CY_CHECK_NEAR(distant, 1.0F, 0.01F);
}

CY_TEST_CASE("cloud shadows: the sun below the horizon casts no shadow, because there is no sun") {
    cy::environment::FieldRegistry registry(allocator());
    CloudShadowField sky;
    CY_REQUIRE(sky.attach(registry, test_quality()));
    cy::environment::FieldStore store(allocator(), registry, partition());

    Clouds overcast;
    CY_REQUIRE(overcast.build(0.95F, 0.8F));

    // The field's value is a FRACTION OF SUNLIGHT. At night there is no sunlight for a cloud to
    // block, so the honest answer is one: a consumer multiplying by it must not darken a scene that
    // is already dark, or dusk goes black twice.
    const Vec3 below = normalize(Vec3{0.4F, -0.5F, 0.1F});
    CY_REQUIRE(sky.update(store, overcast.field, below, 0.0, WorldVec3d{}, 1.0F));
    CY_CHECK_EQ(sky.stats().darkest, 1.0F);
    CY_CHECK_EQ(sky.stats().mean, 1.0F);

    // AND THE CASE THE GUARD IS ACTUALLY FOR, which is not midnight. A sun half a degree ABOVE the
    // horizon gives a shadow ray that climbs to the top of the cloud deck over eight hundred
    // kilometres of nearly horizontal travel — through a map that wraps — and every cell of the
    // field comes out black. That is a world that goes dark at dusk and comes back at dawn with no
    // sunrise in between, and it is invisible in a test that only looks at midnight.
    const Vec3 grazing = normalize(Vec3{0.9999F, 0.01F, 0.0F});
    CY_CHECK_LT(grazing.y, kMinimumSunElevation);
    CY_REQUIRE(sky.update(store, overcast.field, grazing, 0.0, WorldVec3d{}, 1.0F));
    CY_CHECK_EQ(sky.stats().darkest, 1.0F);
    CY_CHECK_EQ(sky.stats().mean, 1.0F);

    // A sun that IS up casts a shadow through the same clouds, so the two checks above are a guard
    // rather than a producer that writes ones.
    const Vec3 risen = normalize(Vec3{0.6F, 0.8F, 0.0F});
    CY_REQUIRE(sky.update(store, overcast.field, risen, 0.0, WorldVec3d{}, 1.0F));
    CY_CHECK_LT(sky.stats().mean, 0.99F);
}

CY_TEST_CASE("cloud shadows: the update rate is a budget lever that can be seen working") {
    cy::environment::FieldRegistry registry(allocator());
    CloudShadowField sky;
    CY_REQUIRE(sky.attach(registry, test_quality()));  // eight hertz
    cy::environment::FieldStore store(allocator(), registry, partition());

    Clouds clouds;
    CY_REQUIRE(clouds.build(0.6F, 0.2F));
    const Vec3 sun = normalize(Vec3{0.2F, 0.9F, 0.1F});

    // Sixteen frames at sixty hertz is 0.266 s, which at eight hertz is two updates and fourteen
    // skips. The count is the lever working; without it, "the update rate is a lever" would be a
    // field somebody set and nothing read.
    u32 wrote = 0;
    for (u32 frame = 0; frame < 16; ++frame) {
        auto result = sky.update(store, clouds.field, sun, static_cast<double>(frame) / 60.0,
                                 WorldVec3d{}, 1.0F / 60.0F);
        CY_REQUIRE(result);
        if (result.value()) {
            ++wrote;
        }
    }
    CY_TEST_MESSAGE(wrote, " updates and ", sky.stats().skipped, " skips over sixteen frames");
    CY_CHECK_EQ(wrote + sky.stats().skipped, 16U);
    CY_CHECK_LE(wrote, 3U);
    CY_CHECK_GE(sky.stats().skipped, 13U);

    // Raising the lever raises the rate, without re-declaring the field: the RESOLUTION is part of
    // the declaration and cannot move at run time, and the update rate is not.
    sky.set_levers(120.0F, 8);
    u32 fast = 0;
    for (u32 frame = 0; frame < 8; ++frame) {
        auto result = sky.update(store, clouds.field, sun, 1.0, WorldVec3d{}, 1.0F / 60.0F);
        CY_REQUIRE(result);
        if (result.value()) {
            ++fast;
        }
    }
    CY_CHECK_EQ(fast, 8U);
}

CY_TEST_CASE("cloud shadows: without a claim, and without clouds, the producer refuses") {
    cy::environment::FieldRegistry registry(allocator());
    cy::environment::FieldStore store(allocator(), registry, partition());

    Clouds clouds;
    CY_REQUIRE(clouds.build(0.5F, 0.0F));

    // A producer that never claimed the field cannot write it. The token is the capability and this
    // is the case that proves it is checked rather than decorative.
    CloudShadowField unclaimed;
    CY_CHECK_FALSE(
        unclaimed.update(store, clouds.field, Vec3{0.0F, 1.0F, 0.0F}, 0.0, WorldVec3d{}, 1.0F));

    CloudShadowField sky;
    CY_REQUIRE(sky.attach(registry, test_quality()));
    CloudField empty;
    empty.layers = default_cloud_layers();
    // No weather map is not "no clouds" — it is a configuration that has not been finished, and
    // writing a field of ones for it would hide the mistake behind a plausible picture.
    CY_CHECK_FALSE(sky.update(store, empty, Vec3{0.0F, 1.0F, 0.0F}, 0.0, WorldVec3d{}, 1.0F));
}
