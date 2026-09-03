#pragma once
// Scalar constants and helpers, and the unit contract everything above this file assumes.
// Task 3.1.1.
//
// `core-math` — "Coordinate conventions": units are **metres, seconds, kilograms and radians**
// internally. Degrees exist only in the editor and in the explicitly named conversion helpers
// below, which is why `radians()` and `degrees()` are the only two functions in the module that
// mention a degree at all. A function taking an angle takes radians and says so in its parameter
// name; there is no overload that takes degrees.
//
// These live in `cy::math` rather than in `cy` because `min`, `max`, `clamp` and `lerp` are names
// a translation unit is likely to have its own spelling of. The types themselves (`Vec3`, `Mat4`,
// `Transform`) are in `cy`, because those names are the engine's and nothing else's.

#include <cy/core/base/types.h>

#include <cmath>
#include <limits>

namespace cy::math {

// --- Constants
// ------------------------------------------------------------------------------------
//
// Spelled as `f32` because that is the engine's runtime precision (`core-math` — "Precision"). The
// `_f64` forms exist for the places the specification names: the simulation clock, accumulated
// time, and interchange helpers.

inline constexpr f32 kPi = 3.14159265358979323846f;
inline constexpr f32 kTwoPi = 6.28318530717958647692f;
inline constexpr f32 kHalfPi = 1.57079632679489661923f;
inline constexpr f32 kQuarterPi = 0.78539816339744830962f;
inline constexpr f32 kInvPi = 0.31830988618379067154f;
inline constexpr f32 kSqrt2 = 1.41421356237309504880f;
inline constexpr f32 kSqrtHalf = 0.70710678118654752440f;

inline constexpr f64 kPi_f64 = 3.14159265358979323846;
inline constexpr f64 kTwoPi_f64 = 6.28318530717958647692;

inline constexpr f32 kDegToRad = kPi / 180.0f;
inline constexpr f32 kRadToDeg = 180.0f / kPi;

/// The tolerance used by `nearly_equal` when a caller does not state one.
///
/// It is deliberately larger than `FLT_EPSILON` (1.19e-7): a comparison worth writing is almost
/// always over a value that has been through a few operations, and a tolerance of one ULP is a
/// test that fails for reasons that have nothing to do with the code under test. A caller that
/// needs an exact comparison writes `==`, which says so.
inline constexpr f32 kEpsilon = 1e-6f;

/// Below this, a vector is treated as having no direction: normalising it would amplify noise into
/// a unit-length answer that means nothing.
inline constexpr f32 kSmallLength = 1e-8f;

inline constexpr f32 kInfinity = std::numeric_limits<f32>::infinity();

// --- Angles
// ---------------------------------------------------------------------------------------

/// Degrees to radians. One of the two places in the engine that may say "degrees".
[[nodiscard]] inline constexpr f32 radians(f32 degrees_value) noexcept {
    return degrees_value * kDegToRad;
}

/// Radians to degrees, for an editor field or a log line. Never for storage.
[[nodiscard]] inline constexpr f32 degrees(f32 radians_value) noexcept {
    return radians_value * kRadToDeg;
}

/// Wrap an angle into (-pi, pi]. Used before comparing two angles or interpolating between them,
/// where the raw difference would otherwise take the long way round.
[[nodiscard]] inline f32 wrap_angle(f32 radians_value) noexcept {
    const f32 wrapped = std::remainder(radians_value, kTwoPi);
    return wrapped;
}

// --- Comparison and interpolation
// -----------------------------------------------------------------

template <typename T>
[[nodiscard]] constexpr T min(T a, T b) noexcept {
    return b < a ? b : a;
}

template <typename T>
[[nodiscard]] constexpr T max(T a, T b) noexcept {
    return a < b ? b : a;
}

template <typename T>
[[nodiscard]] constexpr T clamp(T value, T low, T high) noexcept {
    return value < low ? low : (high < value ? high : value);
}

/// Clamp to [0, 1]. Named for what it is used for rather than for what it does, because that is
/// how it reads at a call site: `saturate(dot(n, l))`.
[[nodiscard]] inline constexpr f32 saturate(f32 value) noexcept {
    return clamp(value, 0.0f, 1.0f);
}

[[nodiscard]] inline constexpr f32 lerp(f32 a, f32 b, f32 t) noexcept {
    return a + (b - a) * t;
}

/// The `t` that `lerp(a, b, t)` would have needed to produce `value`. Returns 0 when a == b, which
/// is the only defensible answer: every `t` is equally correct and 0 is the one that does not
/// depend on floating-point luck.
[[nodiscard]] inline constexpr f32 inverse_lerp(f32 a, f32 b, f32 value) noexcept {
    const f32 span = b - a;
    return span == 0.0f ? 0.0f : (value - a) / span;
}

[[nodiscard]] inline constexpr f32 remap(f32 value, f32 from_low, f32 from_high, f32 to_low,
                                         f32 to_high) noexcept {
    return lerp(to_low, to_high, inverse_lerp(from_low, from_high, value));
}

/// Absolute-difference comparison. `tolerance` is required to have a default rather than to be
/// absent, but a caller comparing values far from 1.0 should state its own: this is an absolute
/// test, and 1e-6 means something quite different next to 1e6 than it does next to 1.
[[nodiscard]] inline bool nearly_equal(f32 a, f32 b, f32 tolerance = kEpsilon) noexcept {
    return std::fabs(a - b) <= tolerance;
}

[[nodiscard]] inline bool nearly_zero(f32 value, f32 tolerance = kEpsilon) noexcept {
    return std::fabs(value) <= tolerance;
}

[[nodiscard]] inline constexpr f32 sign(f32 value) noexcept {
    return value > 0.0f ? 1.0f : (value < 0.0f ? -1.0f : 0.0f);
}

[[nodiscard]] inline bool is_finite(f32 value) noexcept {
    return std::isfinite(value);
}

/// Reciprocal square root, computed exactly rather than approximated.
///
/// The fast approximate form belongs in a SIMD path where its error is measured against the scalar
/// reference (design.md §5), not in the scalar reference itself — the reference is what everything
/// else is compared against, so it is the one implementation that may not cut a corner.
[[nodiscard]] inline f32 rsqrt(f32 value) noexcept {
    return 1.0f / std::sqrt(value);
}

/// A step count for a fixed-step loop: how many whole steps fit in `accumulated`.
///
/// `f64` on purpose. `core-math` — "Precision": the simulation clock and accumulated time are the
/// named exceptions to the engine's 32-bit rule, because a 32-bit accumulator drifts visibly over
/// a session measured in hours.
[[nodiscard]] inline u32 fixed_steps(f64 accumulated_seconds, f64 step_seconds,
                                     u32 max_steps) noexcept {
    if (step_seconds <= 0.0 || accumulated_seconds <= 0.0) {
        return 0;
    }
    const f64 steps = accumulated_seconds / step_seconds;
    if (steps >= static_cast<f64>(max_steps)) {
        return max_steps;
    }
    return static_cast<u32>(steps);
}

}  // namespace cy::math
