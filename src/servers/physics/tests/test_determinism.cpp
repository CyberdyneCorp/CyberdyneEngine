// Determinism across runs on one platform, and the session validation it drives. Task 4.2.6.
//
// WHAT IS CLAIMED, ONCE MORE, BECAUSE THE SCOPE IS THE REQUIREMENT: the same binary, on this
// platform, from the same initial state and the same per-tick inputs, produces identical results.
// Nothing here says anything about another platform, another architecture or another compiler —
// that is M9's deterministic math path, and `physics` requires the limitation to be documented
// rather than quietly assumed away.

#include "fixture.h"

#include <cy/servers/physics/determinism.h>

using namespace cy;
using namespace cy::physics;
using cy::physics::test::Fixture;

namespace {

/// Build the same scene twice and step it, recording a hash per tick.
///
/// `perturb` is what makes the negative control a control: it changes one number on one tick of one
/// run, and everything else about the two runs is identical.
void run(DeterminismProbe& probe, u32 ticks, f32 perturbation = 0.0f) noexcept {
    const Fixture fixture;
    const ShapeHandle sphere = fixture.sphere(0.4f);
    const ShapeHandle box = fixture.box(Vec3{0.4f, 0.4f, 0.4f});
    BodyHandle bodies[6];
    for (u32 index = 0; index < 6; ++index) {
        const Vec3 position{static_cast<f32>(index) * 0.9f, 2.0f + static_cast<f32>(index),
                            static_cast<f32>(index) * -0.3f};
        bodies[index] =
            fixture.body((index % 2) == 0 ? sphere : box, MotionType::Dynamic, position, index + 1);
        // A spin and a shove, so the case exercises the angular integrator and the damping as well
        // as free fall — a hash over six bodies falling straight down would agree between two
        // implementations that differ in everything else.
        CY_REQUIRE(fixture.server
                       ->set_body_velocity(bodies[index],
                                           Vec3{static_cast<f32>(index) * 0.25f, 0.0f, 0.5f},
                                           Vec3{0.3f, static_cast<f32>(index), 0.1f})
                       .has_value());
    }

    for (u32 tick = 0; tick < ticks; ++tick) {
        if (perturbation != 0.0f && tick == ticks / 2) {
            CY_REQUIRE(
                fixture.server->add_impulse(bodies[3], Vec3{perturbation, 0.0f, 0.0f}).has_value());
        }
        CY_REQUIRE(fixture.step(tick).has_value());
        CY_REQUIRE(probe.record(*fixture.server, fixture.world, tick).has_value());
    }
}

}  // namespace

CY_TEST_CASE("two runs of the same scene on the same binary produce identical state hashes") {
    // `physics` — "Replay reproduces a session": "WHEN a recorded input sequence is replayed on the
    // same build and platform THEN the simulation SHALL reproduce the original result".
    DeterminismProbe first(cy::physics::test::allocator(), 128);
    DeterminismProbe second(cy::physics::test::allocator(), 128);
    run(first, 120);
    run(second, 120);

    CY_CHECK_EQ(first.recorded(), 120U);
    CY_CHECK_EQ(second.recorded(), 120U);
    const PhysicsDivergence divergence = DeterminismProbe::compare(first, second);
    CY_CHECK_FALSE(divergence.diverged);
    // The hashes are not merely equal to each other: they are non-zero, so the comparison is over
    // something. Two empty trees also agree.
    CY_CHECK_NE(first.hash_at(119), 0U);
    CY_CHECK_EQ(first.hash_at(119), second.hash_at(119));
}

CY_TEST_CASE("a divergence is reported with the tick and the body that diverged") {
    // `physics` — "Divergence is detected": "WHEN determinism test mode runs and a hash mismatches
    // at tick N THEN the tick number and the diverging body SHALL be reported".
    //
    // THE NEGATIVE CONTROL FOR THE CASE ABOVE. Without it, a probe that hashed nothing would report
    // "no divergence" for two runs that had nothing in common.
    DeterminismProbe first(cy::physics::test::allocator(), 128);
    DeterminismProbe second(cy::physics::test::allocator(), 128);
    run(first, 60);
    run(second, 60, 1.0f);

    const PhysicsDivergence divergence = DeterminismProbe::compare(first, second);
    CY_REQUIRE(divergence.diverged);
    // The impulse lands on tick 30 and the hash is taken after the step, so the very first
    // disagreeing tick is that one.
    CY_CHECK_EQ(divergence.tick, 30U);
    CY_CHECK_FALSE(divergence.shape_mismatch);
    // And it names the body it was applied to — the fourth, created with user data 4. The tree's
    // node id is the body handle, so this is the assertion that the walk is keyed by body rather
    // than by position.
    CY_REQUIRE_FALSE(divergence.body.is_null());
    CY_CHECK_EQ(divergence.body.index(), 3U);
    CY_CHECK_NE(divergence.left_hash, divergence.right_hash);
}

CY_TEST_CASE("a run with an extra body is a shape mismatch, not a value mismatch") {
    const Fixture fixture;
    DeterminismProbe probe(cy::physics::test::allocator(), 8);
    (void)fixture.body(fixture.sphere(0.5f), MotionType::Dynamic, Vec3{}, 1);
    CY_REQUIRE(fixture.step(0).has_value());
    CY_REQUIRE(probe.record(*fixture.server, fixture.world, 0).has_value());

    const Fixture other;
    DeterminismProbe second(cy::physics::test::allocator(), 8);
    (void)other.body(other.sphere(0.5f), MotionType::Dynamic, Vec3{}, 1);
    (void)other.body(other.sphere(0.5f), MotionType::Dynamic, Vec3{5, 0, 0}, 2);
    CY_REQUIRE(other.step(0).has_value());
    CY_REQUIRE(second.record(*other.server, other.world, 0).has_value());

    const PhysicsDivergence divergence = DeterminismProbe::compare(probe, second);
    CY_REQUIRE(divergence.diverged);
    // A run that created one extra entity reports THAT entity rather than every entity after it,
    // which is the whole reason the tree matches children by id.
    CY_CHECK(divergence.shape_mismatch);
}

CY_TEST_CASE("the probe refuses to grow past its capacity rather than exhausting memory") {
    const Fixture fixture;
    DeterminismProbe probe(cy::physics::test::allocator(), 2);
    (void)fixture.body(fixture.sphere(0.5f), MotionType::Dynamic, Vec3{});
    CY_CHECK(probe.record(*fixture.server, fixture.world, 0).has_value());
    CY_CHECK(probe.record(*fixture.server, fixture.world, 1).has_value());
    const Status full = probe.record(*fixture.server, fixture.world, 2);
    CY_REQUIRE_FALSE(full.has_value());
    CY_CHECK_EQ(full.error().code, ErrorCode::OutOfRange);
}

CY_TEST_CASE("a lockstep session with authoritative physics is rejected at configuration time") {
    // `physics`: "A session declaring CrossPlatform or Lockstep while treating physics as
    // authoritative SHALL be rejected at configuration time." A rule that lives only in prose is a
    // rule discovered by a desync months later.
    CY_CHECK_FALSE(validate_session(SessionDeterminism::Lockstep, PhysicsAuthority::Authoritative,
                                    DeterminismPolicy::SamePlatformDeterministic)
                       .has_value());
    CY_CHECK_FALSE(validate_session(SessionDeterminism::CrossPlatform,
                                    PhysicsAuthority::Authoritative,
                                    DeterminismPolicy::SamePlatformDeterministic)
                       .has_value());

    // The escape hatch the requirement names, and it must work: "physics MAY be classified
    // NonAuthoritative and used for debris, ragdolls, and secondary effects outside the
    // deterministic core".
    CY_CHECK(validate_session(SessionDeterminism::Lockstep, PhysicsAuthority::Presentation,
                              DeterminismPolicy::NonAuthoritative)
                 .has_value());

    // Same-platform with a same-platform-deterministic backend is the ordinary case.
    CY_CHECK(validate_session(SessionDeterminism::SamePlatform, PhysicsAuthority::Authoritative,
                              DeterminismPolicy::SamePlatformDeterministic)
                 .has_value());
    // And a backend that does not promise it is refused for that session rather than silently
    // accepted.
    CY_CHECK_FALSE(validate_session(SessionDeterminism::SamePlatform,
                                    PhysicsAuthority::Authoritative,
                                    DeterminismPolicy::NonAuthoritative)
                       .has_value());
}
