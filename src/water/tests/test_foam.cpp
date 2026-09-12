// Foam: persistent, advected, decaying, and bounded. M10 task 2.3, and `water`'s "Foam"
// requirement.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL — reported in `verified_failing`: `FoamField::
// advect()` was changed to write `sample(here) * decay` — decaying in place instead of tracing
// upstream, which is what a foam term that is a function of the surface rather than a field does.
// "a wake persists behind a boat, drifts with the flow and decays" went red on the drift assertion:
// the coverage stayed exactly where it was laid down. The back-trace was restored.

#include <cy/test/test.h>

#include <cy/water/foam.h>

#include "fixtures.h"

namespace test = cy::water::test;
using cy::water::FoamDeposit;
using cy::water::FoamField;
using cy::water::FoamParams;

namespace {

/// A steady eastward current of two metres a second, as a velocity source.
cy::Vec3 eastward(void*, const cy::world::WorldVec3d&) noexcept {
    return cy::Vec3{2.0F, 0.0F, 0.0F};
}

cy::Vec3 still(void*, const cy::world::WorldVec3d&) noexcept {
    return cy::Vec3{0.0F, 0.0F, 0.0F};
}

}  // namespace

CY_TEST_CASE("a wake persists behind a boat, drifts with the flow and decays") {
    FoamField field(test::allocator());
    FoamParams params;
    params.resolution = 64;
    params.cell_metres = 0.5F;
    params.lifetime_seconds = 4.0F;
    CY_REQUIRE(field.configure(params).has_value());
    CY_REQUIRE(field.recentre(cy::world::WorldVec3d{0.0, 0.0, 0.0}).has_value());

    // The boat passes through the origin and keeps going.
    FoamDeposit wake;
    wake.position = cy::world::WorldVec3d{0.0, 0.0, 0.0};
    wake.rate = 1.0F;
    wake.radius = 1.0F;
    CY_REQUIRE(field.deposit(wake, 1.0F).has_value());
    const cy::f32 laid = field.sample(cy::world::WorldVec3d{0.0, 0.0, 0.0});
    CY_CHECK_GT(laid, 0.5F);
    const cy::f32 after_deposit = field.total_coverage();

    // PERSISTENCE: the boat is gone and the foam is not. An instantaneous function of the surface
    // would be zero here.
    CY_REQUIRE(field.advect(0.5F, &still, nullptr).has_value());
    CY_CHECK_GT(field.sample(cy::world::WorldVec3d{0.0, 0.0, 0.0}), 0.3F);

    // DRIFT: with a current, the coverage moves downstream. Two seconds at 2 m/s is four metres.
    CY_REQUIRE(field.recentre(cy::world::WorldVec3d{0.0, 0.0, 0.0}).has_value());
    for (cy::u32 step = 0; step < 8; ++step) {
        CY_REQUIRE(field.advect(0.25F, &eastward, nullptr).has_value());
    }
    const cy::f32 at_origin = field.sample(cy::world::WorldVec3d{0.0, 0.0, 0.0});
    const cy::f32 downstream = field.sample(cy::world::WorldVec3d{4.0, 0.0, 0.0});
    CY_CHECK_GT(downstream, at_origin);

    // DECAY: left alone it fades over the declared lifetime rather than persisting forever.
    for (cy::u32 step = 0; step < 40; ++step) {
        CY_REQUIRE(field.advect(0.5F, &still, nullptr).has_value());
    }
    CY_CHECK_LT(field.total_coverage(), after_deposit * 0.01F);
}

CY_TEST_CASE("the foam field is bounded in memory whatever happens in it") {
    FoamField field(test::allocator());
    FoamParams params;
    params.resolution = 32;
    params.cell_metres = 1.0F;
    CY_REQUIRE(field.configure(params).has_value());
    const cy::u64 bytes = field.bytes();
    CY_CHECK_EQ(bytes, static_cast<cy::u64>(32u * 32u * 2u * sizeof(cy::f32)));

    // A hundred wakes, a thousand advections and a focus dragged across a kilometre: the field is
    // the same size afterwards, because its size is a function of the resolution alone.
    for (cy::u32 step = 0; step < 100; ++step) {
        FoamDeposit deposit;
        deposit.position = cy::world::WorldVec3d{static_cast<cy::f64>(step) * 10.0, 0.0,
                                                 static_cast<cy::f64>(step) * 3.0};
        deposit.rate = 1.0F;
        deposit.radius = 2.0F;
        CY_REQUIRE(field.recentre(deposit.position).has_value());
        CY_REQUIRE(field.deposit(deposit, 0.5F).has_value());
        CY_REQUIRE(field.advect(0.1F, &eastward, nullptr).has_value());
    }
    CY_CHECK_EQ(field.bytes(), bytes);
    CY_CHECK_EQ(field.resolution(), 32u);
}

CY_TEST_CASE("the grid moves in whole cells, so it does not smear under a drifting camera") {
    FoamField field(test::allocator());
    FoamParams params;
    params.resolution = 16;
    params.cell_metres = 2.0F;
    CY_REQUIRE(field.configure(params).has_value());
    CY_REQUIRE(field.recentre(cy::world::WorldVec3d{0.0, 0.0, 0.0}).has_value());

    FoamDeposit deposit;
    deposit.position = cy::world::WorldVec3d{0.0, 0.0, 0.0};
    deposit.rate = 1.0F;
    deposit.radius = 1.0F;
    CY_REQUIRE(field.deposit(deposit, 1.0F).has_value());
    const cy::f32 before = field.sample(cy::world::WorldVec3d{0.0, 0.0, 0.0});

    // A sub-cell move changes nothing at all: no shift, no resampling, no loss.
    CY_REQUIRE(field.recentre(cy::world::WorldVec3d{0.5, 0.0, 0.9}).has_value());
    CY_CHECK_EQ(field.sample(cy::world::WorldVec3d{0.0, 0.0, 0.0}), before);

    // A move of many cells carries the coverage with it, in world terms — the foam stays where it
    // was laid down rather than travelling with the grid.
    CY_REQUIRE(field.recentre(cy::world::WorldVec3d{8.0, 0.0, 0.0}).has_value());
    CY_CHECK_NEAR(field.sample(cy::world::WorldVec3d{0.0, 0.0, 0.0}), before, 1e-5F);

    // And what falls off the edge is dropped rather than wrapping round to the other side.
    CY_REQUIRE(field.recentre(cy::world::WorldVec3d{1000.0, 0.0, 1000.0}).has_value());
    CY_CHECK_NEAR(field.total_coverage(), 0.0F, 1e-5F);
}

CY_TEST_CASE("a field that cannot be built is refused, and an unconfigured one refuses to work") {
    FoamField field(test::allocator());
    FoamParams empty;
    empty.resolution = 0;
    CY_CHECK_FALSE(field.configure(empty).has_value());

    FoamParams eternal;
    eternal.lifetime_seconds = 0.0F;
    CY_CHECK_FALSE(field.configure(eternal).has_value());

    FoamDeposit deposit;
    CY_CHECK_FALSE(field.deposit(deposit, 1.0F).has_value());
    CY_CHECK_FALSE(field.advect(1.0F, &still, nullptr).has_value());
    CY_CHECK_FALSE(field.recentre(cy::world::WorldVec3d{}).has_value());
    CY_CHECK_NEAR(field.sample(cy::world::WorldVec3d{}), 0.0F, 1e-6F);
}

CY_TEST_CASE("a source outside the grid is ignored rather than refused") {
    FoamField field(test::allocator());
    FoamParams params;
    params.resolution = 16;
    params.cell_metres = 1.0F;
    CY_REQUIRE(field.configure(params).has_value());
    CY_REQUIRE(field.recentre(cy::world::WorldVec3d{0.0, 0.0, 0.0}).has_value());

    FoamDeposit distant;
    distant.position = cy::world::WorldVec3d{5000.0, 0.0, 5000.0};
    distant.rate = 1.0F;
    distant.radius = 1.0F;
    // A rapid at the far end of a river is not an error; it is simply not near the focus.
    CY_CHECK(field.deposit(distant, 1.0F).has_value());
    CY_CHECK_NEAR(field.total_coverage(), 0.0F, 1e-6F);
}
