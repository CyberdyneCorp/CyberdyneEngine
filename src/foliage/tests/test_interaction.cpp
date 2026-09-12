// The interaction field: a deterministic drop, a bounded contributor set, and a trail that persists
// and decays. M10 task 2.4; `foliage`'s "Interaction field" requirement.

#include <cy/test/test.h>

#include <cy/foliage/interaction.h>

#include "fixtures.h"

namespace test = cy::foliage::test;
using cy::foliage::InteractionBounds;
using cy::foliage::InteractionEffect;
using cy::foliage::InteractionField;
using cy::foliage::InteractionPrimitive;
using cy::foliage::InteractionSample;
using cy::foliage::InteractionShape;

namespace {

[[nodiscard]] InteractionPrimitive character(cy::u64 source, cy::f64 x, cy::f64 z,
                                             cy::u32 priority) noexcept {
    InteractionPrimitive primitive;
    primitive.source = source;
    primitive.shape = InteractionShape::Sphere;
    primitive.effect = InteractionEffect::Bend;
    primitive.position = cy::world::WorldVec3d{x, 0.0, z};
    primitive.radius_metres = 0.6F;
    primitive.strength = 1.0F;
    primitive.priority = priority;
    return primitive;
}

[[nodiscard]] InteractionBounds small_bounds() noexcept {
    InteractionBounds bounds;
    bounds.extent_metres = 32.0F;
    bounds.max_contributors = 8;
    bounds.trail_cells = 64;
    bounds.trail_lifetime_seconds = 10.0F;
    return bounds;
}

}  // namespace

CY_TEST_CASE("grass parts around a vehicle with no physics body per plant") {
    InteractionField field(test::allocator(), small_bounds());
    CY_REQUIRE(field.recentre(cy::world::WorldVec3d{0.0, 0.0, 0.0}).has_value());

    InteractionPrimitive vehicle;
    vehicle.source = 7;
    vehicle.shape = InteractionShape::Capsule;
    vehicle.effect = InteractionEffect::Displace;
    vehicle.position = cy::world::WorldVec3d{-2.0, 0.0, 0.0};
    vehicle.extent = cy::Vec3{4.0F, 0.0F, 0.0F};
    vehicle.radius_metres = 1.5F;
    vehicle.strength = 1.0F;
    CY_REQUIRE(field.register_primitive(vehicle).has_value());
    auto report = field.resolve(0.016F);
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report.value().admitted, 1u);

    // Beside the capsule: pushed away from it, on the side it is on.
    const InteractionSample left = field.sample(cy::world::WorldVec3d{0.0, 0.0, -1.0});
    const InteractionSample right = field.sample(cy::world::WorldVec3d{0.0, 0.0, 1.0});
    CY_CHECK_EQ(left.contributors, 1u);
    CY_CHECK_EQ(right.contributors, 1u);
    CY_CHECK_LT(left.offset.z, 0.0F);
    CY_CHECK_GT(right.offset.z, 0.0F);

    // Well away from it: untouched.
    const InteractionSample far = field.sample(cy::world::WorldVec3d{0.0, 0.0, 20.0});
    CY_CHECK_EQ(far.contributors, 0u);
    CY_CHECK_EQ(far.offset.z, 0.0F);
}

CY_TEST_CASE("the lowest-priority contributors are dropped, and dropped deterministically") {
    // `foliage` — "a BOUNDED NUMBER OF CONTRIBUTORS, with the lowest-priority contributors DROPPED
    // DETERMINISTICALLY." Registering the same set in two orders must admit the same set, because
    // the trail a contributor deposits is persistent state and is saved.
    InteractionField forward(test::allocator(), small_bounds());
    InteractionField backward(test::allocator(), small_bounds());
    CY_REQUIRE(forward.recentre(cy::world::WorldVec3d{0.0, 0.0, 0.0}).has_value());
    CY_REQUIRE(backward.recentre(cy::world::WorldVec3d{0.0, 0.0, 0.0}).has_value());

    InteractionPrimitive crowd[20];
    for (cy::u32 index = 0; index < 20; ++index) {
        crowd[index] = character(100 + index, static_cast<cy::f64>(index % 5) * 2.0,
                                 static_cast<cy::f64>(index) / 5.0 * 2.0, index % 4);
    }
    for (const InteractionPrimitive& primitive : crowd) {
        CY_REQUIRE(forward.register_primitive(primitive).has_value());
    }
    for (cy::u32 index = 20; index > 0; --index) {
        CY_REQUIRE(backward.register_primitive(crowd[index - 1]).has_value());
    }
    auto a = forward.resolve(0.016F);
    auto b = backward.resolve(0.016F);
    CY_REQUIRE(a.has_value());
    CY_REQUIRE(b.has_value());
    CY_CHECK_EQ(a.value().admitted, 8u);
    CY_CHECK_EQ(a.value().dropped_priority, 12u);
    CY_REQUIRE_EQ(a.value().admitted, b.value().admitted);

    // The same SET, by the caller's own identity, and in the same order.
    cy::u32 differences = 0;
    for (cy::usize index = 0; index < forward.admitted().size(); ++index) {
        differences +=
            forward.admitted()[index].source == backward.admitted()[index].source ? 0U : 1U;
    }
    CY_CHECK_EQ(differences, 0u);

    // And the survivors are the HIGH-priority ones, not merely a stable arbitrary eight: five
    // contributors declared priority 3 and all five are in, the remaining three slots went to
    // priority 2, and nothing below it was admitted at all.
    cy::u32 top = 0;
    cy::u32 second_tier = 0;
    for (const InteractionPrimitive& admitted : forward.admitted()) {
        CY_CHECK_GE(admitted.priority, 2u);
        top += admitted.priority == 3u ? 1U : 0U;
        second_tier += admitted.priority == 2u ? 1U : 0U;
    }
    CY_CHECK_EQ(top, 5u);
    CY_CHECK_EQ(second_tier, 3u);
}

CY_TEST_CASE("a contributor outside the field's extent does not occupy a slot") {
    InteractionField field(test::allocator(), small_bounds());
    CY_REQUIRE(field.recentre(cy::world::WorldVec3d{0.0, 0.0, 0.0}).has_value());
    CY_REQUIRE(field.register_primitive(character(1, 0.0, 0.0, 9)).has_value());
    CY_REQUIRE(field.register_primitive(character(2, 5000.0, 0.0, 99)).has_value());
    auto report = field.resolve(0.016F);
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report.value().dropped_extent, 1u);
    CY_CHECK_EQ(report.value().admitted, 1u);
    CY_CHECK_EQ(field.admitted()[0].source, 1u);
}

CY_TEST_CASE("a trail persists and decays over its declared lifetime") {
    // `foliage` — "Persistent flattening — a trail through grass — SHALL be supported as a DECAYING
    // CONTRIBUTION with a DECLARED LIFETIME."
    InteractionBounds bounds = small_bounds();
    bounds.trail_lifetime_seconds = 4.0F;
    InteractionField field(test::allocator(), bounds);
    CY_REQUIRE(field.recentre(cy::world::WorldVec3d{0.0, 0.0, 0.0}).has_value());

    InteractionPrimitive walker = character(1, 0.0, 0.0, 5);
    walker.effect = InteractionEffect::Flatten;
    walker.radius_metres = 1.2F;
    walker.deposits_trail = true;
    CY_REQUIRE(field.register_primitive(walker).has_value());
    CY_REQUIRE(field.resolve(0.05F).has_value());

    const cy::f32 laid = field.peak_trail();
    CY_CHECK_GT(laid, 0.5F);
    // The character leaves; the trail stays.
    CY_REQUIRE(field.resolve(0.5F).has_value());
    const InteractionSample after_departure = field.sample(cy::world::WorldVec3d{0.0, 0.0, 0.0});
    CY_CHECK_EQ(after_departure.contributors, 0u);
    CY_CHECK_GT(after_departure.trail, 0.4F);

    // And it fades over the declared lifetime rather than at some hard-coded rate.
    for (cy::u32 step = 0; step < 20; ++step) {
        CY_REQUIRE(field.resolve(1.0F).has_value());
    }
    CY_CHECK_LT(field.peak_trail(), laid * 0.05F);
}

CY_TEST_CASE("the trail grid shifts in whole cells, so a track stays where it was laid") {
    InteractionBounds bounds = small_bounds();
    bounds.trail_lifetime_seconds = 1000.0F;
    InteractionField field(test::allocator(), bounds);
    CY_REQUIRE(field.recentre(cy::world::WorldVec3d{0.0, 0.0, 0.0}).has_value());

    InteractionPrimitive stamp = character(1, 4.0, 4.0, 5);
    stamp.effect = InteractionEffect::Flatten;
    stamp.radius_metres = 1.5F;
    stamp.deposits_trail = true;
    CY_REQUIRE(field.register_primitive(stamp).has_value());
    CY_REQUIRE(field.resolve(0.01F).has_value());
    const cy::f32 before = field.sample(cy::world::WorldVec3d{4.0, 0.0, 4.0}).trail;
    CY_REQUIRE((before) > (0.5F));

    // The streaming source walks; the mark must not walk with it.
    const cy::f32 cell = bounds.trail_cell_metres();
    CY_REQUIRE(field
                   .recentre(cy::world::WorldVec3d{static_cast<cy::f64>(cell) * 6.0, 0.0,
                                                   static_cast<cy::f64>(cell) * 6.0})
                   .has_value());
    const cy::f32 after = field.sample(cy::world::WorldVec3d{4.0, 0.0, 4.0}).trail;
    CY_CHECK_GT(after, before * 0.9F);

    // And a shift beyond the grid clears rather than wrapping.
    CY_REQUIRE(field.recentre(cy::world::WorldVec3d{100000.0, 0.0, 100000.0}).has_value());
    CY_CHECK_EQ(field.peak_trail(), 0.0F);
}

CY_TEST_CASE("changing the interaction resolution clears the trail rather than moving it") {
    InteractionField field(test::allocator(), small_bounds());
    CY_REQUIRE(field.recentre(cy::world::WorldVec3d{0.0, 0.0, 0.0}).has_value());
    InteractionPrimitive stamp = character(1, 0.0, 0.0, 5);
    stamp.effect = InteractionEffect::Flatten;
    stamp.deposits_trail = true;
    stamp.radius_metres = 2.0F;
    CY_REQUIRE(field.register_primitive(stamp).has_value());
    CY_REQUIRE(field.resolve(0.01F).has_value());
    CY_REQUIRE((field.peak_trail()) > (0.5F));

    InteractionBounds coarser = small_bounds();
    coarser.trail_cells = 32;
    CY_REQUIRE(field.set_bounds(coarser).has_value());
    // A trail resampled onto a different grid is a trail that MOVED; forgetting it is the lesser
    // failure and it is the one this module chooses, deliberately.
    CY_CHECK_EQ(field.peak_trail(), 0.0F);
    CY_CHECK_EQ(field.bounds().trail_bytes(), 32u * 32u);
}
