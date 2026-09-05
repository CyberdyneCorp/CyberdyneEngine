// Fixed-step integration on the simulation clock, and the interpolation a frame reads.
// Task 4.2.3.

#include "fixture.h"

#include <cy/core/determinism/clock.h>
#include <cy/servers/physics/stepper.h>

using namespace cy;
using namespace cy::physics;
using cy::physics::test::Fixture;

namespace {

determinism::SimulationClock make_clock(u32 rate = 60) noexcept {
    determinism::SimulationClock clock;
    determinism::ClockConfig config;
    config.rate = determinism::TickRate{rate, 1};
    CY_REQUIRE(clock.configure(config).has_value());
    return clock;
}

/// Records what physics published, so a case can check the publication rather than the server.
struct RecordingSink final : TransformSink {
    void publish(BodyHandle body, UserData user_data, const Transform& transform,
                 bool teleported) noexcept override {
        ++published;
        last_body = body;
        last_user_data = user_data;
        last = transform;
        last_teleported = teleported;
    }

    u32 published = 0;
    BodyHandle last_body;
    UserData last_user_data = 0;
    Transform last;
    bool last_teleported = false;
};

}  // namespace

CY_TEST_CASE("a frame with several ticks steps physics once per tick and never in between") {
    // `physics` — "Fixed-step integration": physics "SHALL step exactly once per simulation tick at
    // the fixed timestep... and SHALL never step during a variable-rate frame". The clock is the
    // only source of a delta the stepper has — `SimulationClock` cannot read a wall clock — so the
    // requirement is a property of the wiring, and this is the case that says so out loud.
    const Fixture fixture;
    determinism::SimulationClock clock = make_clock();
    PhysicsStepper stepper(*fixture.server, fixture.world, cy::physics::test::allocator());
    const BodyHandle body = fixture.body(fixture.sphere(0.5f), MotionType::Dynamic, Vec3{});
    CY_REQUIRE(stepper.track(body).has_value());

    // A long frame: 100 ms at 60 Hz is six ticks.
    const determinism::FrameTicks ticks = clock.accumulate(100'000'000);
    CY_CHECK_EQ(ticks.ticks, 6U);
    for (u32 index = 0; index < ticks.ticks; ++index) {
        CY_REQUIRE(stepper.tick(clock, nullptr).has_value());
        clock.advance();
    }
    CY_CHECK_EQ(stepper.steps(), 6U);

    const Expected<StepStatistics, Error> statistics = fixture.server->statistics(fixture.world);
    CY_REQUIRE(statistics.has_value());
    // The tick the server recorded is the CLOCK's, not a counter the stepper kept: a stepper that
    // invented its own numbering would still report six steps.
    CY_CHECK_EQ(statistics->tick, 5U);

    // A frame that contains no tick steps nothing at all.
    const determinism::FrameTicks none = clock.accumulate(1'000'000);
    CY_CHECK_EQ(none.ticks, 0U);
    CY_CHECK_EQ(stepper.steps(), 6U);
}

CY_TEST_CASE("rendering interpolates between the previous and the current physics transform") {
    // `physics` — "Rendering interpolates physics": above the physics rate, rendered positions
    // "SHALL interpolate between physics ticks rather than stepping".
    const Fixture fixture;
    determinism::SimulationClock clock = make_clock();
    PhysicsStepper stepper(*fixture.server, fixture.world, cy::physics::test::allocator());
    const BodyHandle body =
        fixture.body(fixture.box(Vec3{0.5f, 0.5f, 0.5f}), MotionType::Kinematic, Vec3{});
    CY_REQUIRE(
        fixture.server->set_body_velocity(body, Vec3{60.0f, 0.0f, 0.0f}, Vec3{}).has_value());
    CY_REQUIRE(stepper.track(body).has_value());

    (void)clock.accumulate(determinism::step_nanoseconds(clock.rate()) * 2);
    CY_REQUIRE(stepper.tick(clock, nullptr).has_value());
    clock.advance();
    CY_REQUIRE(stepper.tick(clock, nullptr).has_value());

    // One metre per tick at 60 m/s and 60 Hz: the pair spans exactly one metre.
    const Expected<Transform, Error> start = stepper.interpolate(body, 0.0f);
    const Expected<Transform, Error> middle = stepper.interpolate(body, 0.5f);
    const Expected<Transform, Error> end = stepper.interpolate(body, 1.0f);
    CY_REQUIRE(start.has_value());
    CY_REQUIRE(middle.has_value());
    CY_REQUIRE(end.has_value());
    CY_CHECK_NEAR(end->translation.x - start->translation.x, 1.0f, 1e-3f);
    CY_CHECK_NEAR(middle->translation.x, (start->translation.x + end->translation.x) * 0.5f, 1e-3f);
}

CY_TEST_CASE("a teleported body is not interpolated across the discontinuity") {
    // `physics` — "Teleport suppresses interpolation": the flag suppresses interpolation for that
    // frame, "avoiding a smear across the world". Checked at alpha 0, which is where the obvious
    // implementation — clamp then lerp — returns the position the body was teleported AWAY from.
    const Fixture fixture;
    determinism::SimulationClock clock = make_clock();
    PhysicsStepper stepper(*fixture.server, fixture.world, cy::physics::test::allocator());
    const BodyHandle body = fixture.body(fixture.sphere(0.5f), MotionType::Kinematic, Vec3{});
    CY_REQUIRE(stepper.track(body).has_value());

    (void)clock.accumulate(determinism::step_nanoseconds(clock.rate()) * 2);
    CY_REQUIRE(stepper.tick(clock, nullptr).has_value());
    clock.advance();

    CY_REQUIRE(fixture.server
                   ->set_body_transform(body,
                                        Transform::from_translation(Vec3{1000.0f, 0.0f, 0.0f}),
                                        TeleportMode::Teleport)
                   .has_value());
    RecordingSink sink;
    CY_REQUIRE(stepper.tick(clock, &sink).has_value());

    CY_CHECK(sink.last_teleported);
    const Expected<Transform, Error> at_zero = stepper.interpolate(body, 0.0f);
    CY_REQUIRE(at_zero.has_value());
    CY_CHECK_NEAR(at_zero->translation.x, 1000.0f, 1e-3f);

    // The following tick is a normal one again: the flag lasts exactly the step it belongs to.
    clock.advance();
    CY_REQUIRE(stepper.tick(clock, &sink).has_value());
    CY_CHECK_FALSE(sink.last_teleported);
}

CY_TEST_CASE("the stepper publishes every tracked body's transform and its user data") {
    const Fixture fixture;
    determinism::SimulationClock clock = make_clock();
    PhysicsStepper stepper(*fixture.server, fixture.world, cy::physics::test::allocator());
    const BodyHandle body = fixture.body(fixture.sphere(0.5f), MotionType::Dynamic, Vec3{}, 77);
    CY_REQUIRE(stepper.track(body).has_value());
    // Tracking twice is idempotent: a bridge that re-registers on a component change must not
    // publish the same body twice per tick.
    CY_REQUIRE(stepper.track(body).has_value());
    CY_CHECK_EQ(stepper.tracked_count(), 1U);

    RecordingSink sink;
    CY_REQUIRE(stepper.tick(clock, &sink).has_value());
    CY_CHECK_EQ(sink.published, 1U);
    CY_CHECK_EQ(sink.last_user_data, 77U);
    CY_CHECK_EQ(sink.last_body.bits(), body.bits());

    stepper.untrack(body);
    CY_CHECK_EQ(stepper.tracked_count(), 0U);
    CY_REQUIRE(stepper.tick(clock, &sink).has_value());
    CY_CHECK_EQ(sink.published, 1U);
    CY_CHECK_FALSE(stepper.interpolate(body, 0.5f).has_value());
}

CY_TEST_CASE("a body tracked mid-session interpolates to a standstill, not from the origin") {
    // Both halves of the pair start at the current transform. A `Transform{}` previous would make
    // the body's first rendered frame a streak from the world origin.
    const Fixture fixture;
    PhysicsStepper stepper(*fixture.server, fixture.world, cy::physics::test::allocator());
    const BodyHandle body =
        fixture.body(fixture.sphere(0.5f), MotionType::Kinematic, Vec3{5.0f, 6.0f, 7.0f});
    CY_REQUIRE(stepper.track(body).has_value());
    const Expected<Transform, Error> at_zero = stepper.interpolate(body, 0.0f);
    CY_REQUIRE(at_zero.has_value());
    CY_CHECK_EQ(at_zero->translation.x, 5.0f);
    CY_CHECK_EQ(at_zero->translation.z, 7.0f);
}
