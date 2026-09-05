// Frame-rate independent smoothing. Task 4.3.2.
//
// `camera-system` — "Stable smoothing": smoothing "SHALL produce the same visual result at
// different frame rates", and "Naive per-frame interpolation toward a target SHALL NOT be used,
// because its behaviour depends on frame rate."
//
// THE CASE THAT MATTERS IS THE COMPOSITION ONE. A test that pinned the residue after a single step
// would pass for `lerp(current, target, 0.1F)` as well, because a single step of any formulation
// can be made to hit any number. What distinguishes the two is that the exponential form composes:
// two steps of dt leave the same residue as one step of 2·dt, and the naive form does not. That is
// asserted directly below, and again as the property a designer actually feels — the same settle
// time at 60 and at 144 hertz.

#include <cy/servers/camera/smoothing.h>
#include <cy/test/test.h>

#include <cmath>

using cy::f32;
using namespace cy::camera;

CY_TEST_CASE("a half-life means what it says: half the gap closes in one half-life") {
    SmoothScalar value(0.0F);
    (void)value.advance(1.0F, 0.25F, 0.25F);
    CY_CHECK_NEAR(value.value(), 0.5F, 1e-5F);
    (void)value.advance(1.0F, 0.25F, 0.25F);
    CY_CHECK_NEAR(value.value(), 0.75F, 1e-5F);
}

CY_TEST_CASE("two steps of dt leave the same residue as one step of twice dt") {
    // THE PROPERTY THE NAIVE FORM DOES NOT HAVE. `lerp(current, target, k)` twice leaves
    // (1-k)^2 = 0.81 of the error for k = 0.1, while one step of a doubled k leaves 0.8 — close
    // enough to look right at one frame rate and to be visibly different at another.
    SmoothScalar stepped(0.0F);
    (void)stepped.advance(1.0F, 0.2F, 1.0F / 120.0F);
    (void)stepped.advance(1.0F, 0.2F, 1.0F / 120.0F);

    SmoothScalar once(0.0F);
    (void)once.advance(1.0F, 0.2F, 1.0F / 60.0F);

    CY_CHECK_NEAR(stepped.value(), once.value(), 1e-6F);
}

CY_TEST_CASE("the same scene settles over the same wall-clock time at 60 and at 144 hertz") {
    // `camera-system`'s own scenario, run rather than reasoned about.
    const auto settle = [](f32 rate) {
        SmoothScalar value(0.0F);
        const f32 step = 1.0F / rate;
        const auto frames = static_cast<int>(rate);  // exactly one second
        for (int i = 0; i < frames; ++i) {
            (void)value.advance(1.0F, 0.15F, step);
        }
        return value.value();
    };
    CY_CHECK_NEAR(settle(60.0F), settle(144.0F), 1e-4F);
    CY_CHECK_NEAR(settle(30.0F), settle(240.0F), 1e-4F);
}

CY_TEST_CASE("a non-positive half-life takes the target and a non-positive delta keeps the value") {
    CY_CHECK_NEAR(decay_fraction(0.0F, 0.016F), 1.0F, 1e-6F);
    CY_CHECK_NEAR(decay_fraction(-1.0F, 0.016F), 1.0F, 1e-6F);
    CY_CHECK_NEAR(decay_fraction(0.2F, 0.0F), 0.0F, 1e-6F);
    CY_CHECK_NEAR(decay_fraction(0.2F, -1.0F), 0.0F, 1e-6F);
}

CY_TEST_CASE("a reset discards the smoother's history, which is what a cut needs") {
    SmoothVec3 position(cy::Vec3{0.0F, 0.0F, 0.0F});
    (void)position.advance(cy::Vec3{10.0F, 0.0F, 0.0F}, 0.2F, 0.1F);
    CY_CHECK_GT(position.value().x, 0.0F);
    CY_CHECK_LT(position.value().x, 10.0F);

    position.reset(cy::Vec3{100.0F, 0.0F, 0.0F});
    CY_CHECK_NEAR(position.value().x, 100.0F, 1e-6F);
    // And the next advance eases from the reset value, not from where it was before.
    (void)position.advance(cy::Vec3{100.0F, 0.0F, 0.0F}, 0.2F, 0.1F);
    CY_CHECK_NEAR(position.value().x, 100.0F, 1e-4F);
}

CY_TEST_CASE("an orientation smoother stays a unit quaternion and takes the short way round") {
    SmoothQuat rotation(cy::Quat::identity());
    const cy::Quat target = cy::Quat::from_axis_angle(cy::kAxisUp, 3.0F);  // most of a half turn
    // Two seconds at a 0.1 s half-life is twenty half-lives, so the residue is 3·2⁻²⁰ radians.
    for (int i = 0; i < 120; ++i) {
        (void)rotation.advance(target, 0.1F, 1.0F / 60.0F);
    }
    const cy::Quat& value = rotation.value();
    const f32 norm = std::sqrt((value.x * value.x) + (value.y * value.y) + (value.z * value.z) +
                               (value.w * value.w));
    CY_CHECK_NEAR(norm, 1.0F, 1e-4F);
    // A per-component exponential decay would have taken the chord and denormalised; slerp arrives.
    CY_CHECK_NEAR(cy::angle_between(value, target), 0.0F, 1e-3F);
}
