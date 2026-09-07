// Unified render importance: one value per instance, and declared transforms rather than second
// opinions. M6 task 4.1.
//
// `residency` — "Unified render importance": "Subsystems SHALL NOT maintain independent notions of
// importance for the same instance. Where a subsystem needs a different weighting, it SHALL apply a
// declared transform of the shared value rather than compute its own." The two scenarios below the
// requirement are "A hero unit is important once" and "Weighting, not redefinition", and both are
// cases here.

#include <cy/servers/residency/importance.h>
#include <cy/test/test.h>

#include <limits>

using namespace cy::residency;
using cy::f32;

namespace {

[[nodiscard]] ImportanceInputs inputs(f32 coverage, f32 gameplay, f32 distance) noexcept {
    ImportanceInputs value;
    value.screen_coverage = coverage;
    value.gameplay_importance = gameplay;
    value.distance_normalised = distance;
    return value;
}

}  // namespace

CY_TEST_CASE("coverage enters as extent, not as area") {
    // A quad covering a quarter of the screen is half as wide as one covering all of it. Scoring it
    // at a quarter would put every mid-distance instance two orders of magnitude below a near one.
    CY_CHECK_NEAR(compute_render_importance(inputs(0.25F, 0.0F, 0.0F)), 0.5F, 1e-5F);
    CY_CHECK_NEAR(compute_render_importance(inputs(1.0F, 0.0F, 0.0F)), 1.0F, 1e-5F);
    CY_CHECK_NEAR(compute_render_importance(inputs(0.0F, 0.0F, 0.0F)), 0.0F, 1e-5F);
}

CY_TEST_CASE("the gameplay mark raises importance rather than averaging with it") {
    // A hero unit filling one percent of the screen is a hero unit. An average would answer "half
    // a hero", which is the answer that sends gameplay code off to invent its own importance.
    const f32 marked = compute_render_importance(inputs(0.01F, 1.0F, 0.0F));
    CY_CHECK_NEAR(marked, 1.0F, 1e-5F);
    CY_CHECK_GT(marked, compute_render_importance(inputs(0.01F, 0.0F, 0.0F)));
}

CY_TEST_CASE("a NaN or a negative coverage cannot become an importance") {
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    CY_CHECK_NEAR(compute_render_importance(inputs(nan, nan, 0.0F)), 0.0F, 1e-5F);
    CY_CHECK_NEAR(compute_render_importance(inputs(-4.0F, -1.0F, 0.0F)), 0.0F, 1e-5F);
    CY_CHECK_NEAR(compute_render_importance(inputs(4.0F, 4.0F, 0.0F)), 1.0F, 1e-5F);
}

CY_TEST_CASE("a hero unit is important once: one declaration reaches every consumer") {
    ImportanceTable table;
    CY_REQUIRE(table.publish(7, inputs(0.04F, 1.0F, 0.5F)));

    // Nothing else was published, and every subsystem reads the same instance.
    for (cy::u32 index = 0; index < kSubsystemCount; ++index) {
        const auto subsystem = static_cast<Subsystem>(index);
        CY_CHECK_NEAR(table.for_subsystem(7, subsystem), table.shared(7), 1e-5F);
    }
    CY_CHECK_EQ(table.size(), 1U);
}

CY_TEST_CASE("weighting, not redefinition: shadows weight distance more strongly than textures") {
    ImportanceTable table;
    ImportanceTransform shadows;
    shadows.distance_weight = 0.9F;  // `residency`'s own example
    CY_REQUIRE(table.declare_transform(Subsystem::Shadow, shadows));

    CY_REQUIRE(table.publish(1, inputs(1.0F, 0.0F, 0.0F)));  // near
    CY_REQUIRE(table.publish(2, inputs(1.0F, 0.0F, 1.0F)));  // far

    // The shared value is identical: distance is not part of it.
    CY_CHECK_NEAR(table.shared(1), table.shared(2), 1e-5F);

    // Textures, with no declaration, read the shared value for both.
    CY_CHECK_NEAR(table.for_subsystem(1, Subsystem::Texture),
                  table.for_subsystem(2, Subsystem::Texture), 1e-5F);

    // Shadows apply the declared transform, and only to the far one.
    CY_CHECK_NEAR(table.for_subsystem(1, Subsystem::Shadow), 1.0F, 1e-5F);
    CY_CHECK_NEAR(table.for_subsystem(2, Subsystem::Shadow), 0.1F, 1e-5F);
}

CY_TEST_CASE("an exponent that would invert the ordering is refused") {
    ImportanceTable table;
    ImportanceTransform broken;
    broken.exponent = 0.0F;
    CY_CHECK_FALSE(table.declare_transform(Subsystem::Geometry, broken));
    broken.exponent = -1.0F;
    CY_CHECK_FALSE(table.declare_transform(Subsystem::Geometry, broken));
    broken.exponent = 2.0F;
    CY_CHECK(table.declare_transform(Subsystem::Geometry, broken));
}

CY_TEST_CASE("an instance nobody published is zero, not a guess") {
    ImportanceTable table;
    CY_CHECK_NEAR(table.shared(99), 0.0F, 1e-6F);
    CY_CHECK_NEAR(table.for_subsystem(99, Subsystem::Texture), 0.0F, 1e-6F);
    CY_CHECK(table.inputs(99) == nullptr);

    CY_REQUIRE(table.publish(99, inputs(1.0F, 0.0F, 0.0F)));
    CY_CHECK(table.retire(99));
    CY_CHECK_FALSE(table.retire(99));
    CY_CHECK_NEAR(table.shared(99), 0.0F, 1e-6F);
}
