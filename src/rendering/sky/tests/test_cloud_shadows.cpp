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
    f32 lowest = 1.0F;
    f32 highest = 0.0F;
    for (cy::i32 step = -3; step <= 3; ++step) {
        const f32 sampled = CloudShadowField::sample(
            store,
            WorldVec3d{static_cast<double>(step) * 128.0, 0.0, static_cast<double>(step) * 128.0});
        CY_CHECK_GE(sampled, 0.0F);
        CY_CHECK_LE(sampled, 1.0F);
        lowest = cy::math::min(lowest, sampled);
        highest = cy::math::max(highest, sampled);
    }
    CY_CHECK_GE(highest, lowest);

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
