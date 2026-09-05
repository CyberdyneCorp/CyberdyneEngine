// Frame-rate independent smoothing. See cy/servers/camera/smoothing.h.

#include <cy/servers/camera/smoothing.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::camera {

f32 decay_fraction(f32 half_life_seconds, f32 delta_seconds) noexcept {
    // A non-positive half-life is "no smoothing": take the target. A non-positive delta is "no time
    // passed": keep the current value. Both are legitimate calls — a rig parameter left at zero, a
    // paused frame — and both have an obvious right answer, so neither is an assertion.
    if (half_life_seconds <= 0.0F) {
        return 1.0F;
    }
    if (delta_seconds <= 0.0F) {
        return 0.0F;
    }
    // 1 - 2^(-dt/h). std::exp2 rather than std::pow: it is the operation, and it composes exactly
    // the way the header comment claims.
    return 1.0F - std::exp2(-delta_seconds / half_life_seconds);
}

Vec3 SmoothVec3::advance(Vec3 target, f32 half_life_seconds, f32 delta_seconds) noexcept {
    const f32 fraction = decay_fraction(half_life_seconds, delta_seconds);
    value_ = value_ + ((target - value_) * fraction);
    return value_;
}

const Quat& SmoothQuat::advance(const Quat& target, f32 half_life_seconds,
                                f32 delta_seconds) noexcept {
    const f32 fraction = decay_fraction(half_life_seconds, delta_seconds);
    value_ = slerp(value_, target, fraction);
    return value_;
}

}  // namespace cy::camera
