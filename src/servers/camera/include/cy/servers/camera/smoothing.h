#pragma once
// Frame-rate independent smoothing, expressed as a half-life. Task 4.3.2.
//
// `camera-system` — "Stable smoothing": smoothing "SHALL use **frame-rate independent**
// formulations expressed in physically meaningful terms — half-life, frequency and damping ratio,
// or an equivalent — and SHALL produce the same visual result at different frame rates. Naive
// per-frame interpolation toward a target SHALL NOT be used, because its behaviour depends on frame
// rate." It also requires that smoothing state "SHALL be resettable on cuts and teleports".
//
// --- THE ONE LINE THAT MATTERS ------------------------------------------------------------------
//
//     current += (target - current) * (1 - 2^(-dt / half_life))
//
// `1 - 2^(-dt/h)` is the fraction of the remaining error that decays in `dt` seconds when the error
// halves every `h` seconds. It composes: two steps of dt and one step of 2·dt leave the same
// residue, which is exactly the property `lerp(current, target, 0.1F)` per frame does not have — at
// 144 hertz that form converges 2.4 times faster than at 60, so a camera tuned on one machine feels
// wrong on another. `tests/test_smoothing.cpp` asserts the composition directly rather than
// asserting a number: a test that pinned the residue after one step would still pass for the naive
// form.
//
// A HALF-LIFE, NOT A "SMOOTHING FACTOR". Half-life is in seconds and a designer can reason about
// it: 0.1 means the camera has closed half the gap after 100 milliseconds. A factor is a number
// that only means anything alongside the frame rate it was chosen at, which is how the
// frame-rate-dependent form gets written in the first place.

#include <cy/core/base/types.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/vec.h>

namespace cy::camera {

/// The fraction of the remaining error that decays over `delta_seconds` at this half-life.
///
/// A non-positive half-life means "no smoothing", and returns 1 — the target, immediately. That is
/// the sensible reading and it keeps a zeroed parameter block from freezing a camera in place.
[[nodiscard]] f32 decay_fraction(f32 half_life_seconds, f32 delta_seconds) noexcept;

/// A smoothed scalar. `reset()` is what a cut calls.
class SmoothScalar {
public:
    SmoothScalar() = default;
    explicit SmoothScalar(f32 initial) noexcept : value_(initial) {}

    [[nodiscard]] f32 value() const noexcept { return value_; }
    void reset(f32 value) noexcept { value_ = value; }

    f32 advance(f32 target, f32 half_life_seconds, f32 delta_seconds) noexcept {
        value_ += (target - value_) * decay_fraction(half_life_seconds, delta_seconds);
        return value_;
    }

private:
    f32 value_ = 0.0F;
};

class SmoothVec3 {
public:
    SmoothVec3() = default;
    explicit SmoothVec3(Vec3 initial) noexcept : value_(initial) {}

    [[nodiscard]] Vec3 value() const noexcept { return value_; }
    void reset(Vec3 value) noexcept { value_ = value; }

    Vec3 advance(Vec3 target, f32 half_life_seconds, f32 delta_seconds) noexcept;

private:
    Vec3 value_{0.0F, 0.0F, 0.0F};
};

/// A smoothed orientation.
///
/// `slerp` rather than a component-wise decay: `camera-system` requires that "Rotation blending
/// SHALL interpolate correctly for orientations", and a per-component exponential decay on a
/// quaternion is not a rotation path — it takes the chord rather than the arc and denormalises on
/// the way. The decay fraction is the same one, so the frame-rate independence is the same.
class SmoothQuat {
public:
    SmoothQuat() = default;
    explicit SmoothQuat(const Quat& initial) noexcept : value_(initial) {}

    [[nodiscard]] const Quat& value() const noexcept { return value_; }
    void reset(const Quat& value) noexcept { value_ = value; }

    const Quat& advance(const Quat& target, f32 half_life_seconds, f32 delta_seconds) noexcept;

private:
    Quat value_ = Quat::identity();
};

}  // namespace cy::camera
