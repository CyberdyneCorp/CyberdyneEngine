// SPDX-License-Identifier: MIT
#pragma once
// Vectors, planar rotations, quaternions and transforms over `Fixed`. Design §6.
//
// THE CONVENTIONS ARE core-math's: right-handed, Y up, -Z forward, a rotation counter-clockwise
// about an axis seen from its positive end. tests/test_conventions.cpp asserts the same numeric
// consequences src/core/math/tests/test_conventions.cpp asserts for `Vec3` and `Quat`, so both
// libraries name the same direction "forward".
//
// EVERY PRODUCT-SHAPED QUERY RETURNS A `WideFixed`. `dot`, `cross` of two planar vectors and
// `length_squared` are exact in Q64.64, so comparing two squared distances never overflows and
// never rounds. A value comes back to `Fixed` once, at a stated rounding:
//
//   v * k              each component by `Fixed *` (nearest, ties toward +infinity)
//   length, distance   the correctly rounded square root of the exact Q64.64 sum
//   normalize          each component divided by `length` (`Fixed /`, truncated); the zero vector
//                      gives zero and is counted
//   clamp_length       each component times the limit over `length`, from the exact product
//                      (`mul_div`, truncated)
//   cross (3D)         each component's exact Q64.64 difference, rounded once by `narrow()`
//   Rot2, FixedQuat    each output component's exact Q64.64 sum of products, rounded once
//
// `FixedQuat` has no `slerp`: interpolation is presentation and happens in `f32` after conversion
// (design §6). There is no matrix type either; a kernel that needs a rotation matrix derives it
// from a quaternion where it needs it.

#include <cy/core/base/types.h>
#include <cy/core/detmath/config.h>
#include <cy/core/detmath/fixed.h>

namespace cy::detmath::inline CY_DETMATH_VARIANT {

/// `a * b / c` from the exact 128-bit product, truncated toward zero, wrapped. Division by zero
/// gives `Fixed::max()` when `a * b >= 0` and `Fixed::min()` otherwise, as `/` does, and is
/// counted. The scaling a vector needs — a component times a new length over its old one — without
/// the rounding of an intermediate `Fixed`.
[[nodiscard]] Fixed mul_div(Fixed a, Fixed b, Fixed c) noexcept;

// --- FixedVec2
// ------------------------------------------------------------------------------------

/// A planar vector. Which world plane it lies in is the user's: the kinematic mover reads `x` as
/// world X and `y` as world Z.
struct FixedVec2 {
    Fixed x;
    Fixed y;

    [[nodiscard]] static constexpr FixedVec2 zero() noexcept { return {}; }

    /// Raw equality, component by component.
    friend constexpr bool operator==(const FixedVec2&, const FixedVec2&) = default;
};

[[nodiscard]] constexpr FixedVec2 operator+(FixedVec2 a, FixedVec2 b) noexcept {
    return {a.x + b.x, a.y + b.y};
}
[[nodiscard]] constexpr FixedVec2 operator-(FixedVec2 a, FixedVec2 b) noexcept {
    return {a.x - b.x, a.y - b.y};
}
[[nodiscard]] constexpr FixedVec2 operator-(FixedVec2 a) noexcept {
    return {-a.x, -a.y};
}
[[nodiscard]] inline FixedVec2 operator*(FixedVec2 v, Fixed k) noexcept {
    return {v.x * k, v.y * k};
}
constexpr FixedVec2& operator+=(FixedVec2& a, FixedVec2 b) noexcept {
    return a = a + b;
}
constexpr FixedVec2& operator-=(FixedVec2& a, FixedVec2 b) noexcept {
    return a = a - b;
}

/// `a . b`, exactly.
[[nodiscard]] inline WideFixed dot(FixedVec2 a, FixedVec2 b) noexcept {
    return WideFixed::product(a.x, b.x) + WideFixed::product(a.y, b.y);
}

/// `a.x b.y - a.y b.x`, exactly: positive when `b` is counter-clockwise of `a` in the (x, y) plane.
/// The sign is what a funnel and a point-in-polygon test decide on, so it is never rounded.
[[nodiscard]] inline WideFixed cross(FixedVec2 a, FixedVec2 b) noexcept {
    return WideFixed::product(a.x, b.y) - WideFixed::product(a.y, b.x);
}

/// `|v|^2`, exactly.
[[nodiscard]] inline WideFixed length_squared(FixedVec2 v) noexcept {
    return dot(v, v);
}

/// `|v|`, correctly rounded.
[[nodiscard]] Fixed length(FixedVec2 v) noexcept;

/// `|b - a|`, correctly rounded. The difference wraps when the points are more than 2^31 apart.
[[nodiscard]] Fixed distance(FixedVec2 a, FixedVec2 b) noexcept;

/// `|b - a|^2`, exactly.
[[nodiscard]] inline WideFixed distance_squared(FixedVec2 a, FixedVec2 b) noexcept {
    return length_squared(b - a);
}

/// `v / |v|`, each component truncated toward zero. The zero vector gives zero and is counted.
[[nodiscard]] FixedVec2 normalize(FixedVec2 v) noexcept;

/// `v` scaled to length `limit` (a non-negative value) when it is longer. Unchanged when it is not,
/// which is decided exactly on the squares. Each scaled component is within an ulp of its exact
/// value, since `length` is correctly rounded and `mul_div` truncates once.
[[nodiscard]] FixedVec2 clamp_length(FixedVec2 v, Fixed limit) noexcept;

// --- FixedVec3
// ------------------------------------------------------------------------------------

/// A vector in world space. 24 bytes: `Fixed16Vec3` is the 12-byte storage form.
struct FixedVec3 {
    Fixed x;
    Fixed y;
    Fixed z;

    [[nodiscard]] static constexpr FixedVec3 zero() noexcept { return {}; }

    friend constexpr bool operator==(const FixedVec3&, const FixedVec3&) = default;
};

/// The axes, in core-math's names. `kAxis*` in src/core/math are the same directions.
[[nodiscard]] constexpr FixedVec3 fixed_axis_x() noexcept {
    return {Fixed::one(), Fixed::zero(), Fixed::zero()};
}
[[nodiscard]] constexpr FixedVec3 fixed_axis_y() noexcept {
    return {Fixed::zero(), Fixed::one(), Fixed::zero()};
}
[[nodiscard]] constexpr FixedVec3 fixed_axis_z() noexcept {
    return {Fixed::zero(), Fixed::zero(), Fixed::one()};
}
/// -Z: what an identity rotation faces.
[[nodiscard]] constexpr FixedVec3 fixed_forward() noexcept {
    return {Fixed::zero(), Fixed::zero(), -Fixed::one()};
}

[[nodiscard]] constexpr FixedVec3 operator+(FixedVec3 a, FixedVec3 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
[[nodiscard]] constexpr FixedVec3 operator-(FixedVec3 a, FixedVec3 b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
[[nodiscard]] constexpr FixedVec3 operator-(FixedVec3 a) noexcept {
    return {-a.x, -a.y, -a.z};
}
[[nodiscard]] inline FixedVec3 operator*(FixedVec3 v, Fixed k) noexcept {
    return {v.x * k, v.y * k, v.z * k};
}
constexpr FixedVec3& operator+=(FixedVec3& a, FixedVec3 b) noexcept {
    return a = a + b;
}
constexpr FixedVec3& operator-=(FixedVec3& a, FixedVec3 b) noexcept {
    return a = a - b;
}

/// `a . b`, exactly.
[[nodiscard]] inline WideFixed dot(FixedVec3 a, FixedVec3 b) noexcept {
    return WideFixed::product(a.x, b.x) + WideFixed::product(a.y, b.y) +
           WideFixed::product(a.z, b.z);
}

/// `a x b`, each component rounded once from its exact value.
[[nodiscard]] FixedVec3 cross(FixedVec3 a, FixedVec3 b) noexcept;

/// `|v|^2`, exactly.
[[nodiscard]] inline WideFixed length_squared(FixedVec3 v) noexcept {
    return dot(v, v);
}

/// `|v|`, correctly rounded.
[[nodiscard]] Fixed length(FixedVec3 v) noexcept;

/// `|b - a|`, correctly rounded.
[[nodiscard]] Fixed distance(FixedVec3 a, FixedVec3 b) noexcept;

/// `v / |v|`, each component truncated toward zero. The zero vector gives zero and is counted.
[[nodiscard]] FixedVec3 normalize(FixedVec3 v) noexcept;

// --- Fixed16Vec3
// ----------------------------------------------------------------------------------

/// The 12-byte storage form of a `FixedVec3`, for dense data whose range fits ±32 768 (design
/// §4.2). Never computed with: `widen()` it.
struct Fixed16Vec3 {
    Fixed16 x;
    Fixed16 y;
    Fixed16 z;

    /// Each component rounded to nearest, ties toward +infinity; out of range saturates and is
    /// counted.
    [[nodiscard]] static constexpr Fixed16Vec3 narrow(FixedVec3 v) noexcept {
        return {Fixed16::narrow(v.x), Fixed16::narrow(v.y), Fixed16::narrow(v.z)};
    }
    /// The same value as a `FixedVec3`. Exact.
    [[nodiscard]] constexpr FixedVec3 widen() const noexcept {
        return {x.widen(), y.widen(), z.widen()};
    }

    friend constexpr bool operator==(const Fixed16Vec3&, const Fixed16Vec3&) = default;
};

// --- Rot2
// -----------------------------------------------------------------------------------------

/// A planar rotation, as its cosine and sine. What an RTS heading is, rather than a quaternion.
struct Rot2 {
    Fixed cos = Fixed::one();
    Fixed sin = Fixed::zero();

    [[nodiscard]] static constexpr Rot2 identity() noexcept { return {}; }
    /// The rotation by `angle`, counter-clockwise in the (x, y) plane: `sincos`, so within 1 ulp
    /// per component.
    [[nodiscard]] static Rot2 from_angle(Angle angle) noexcept;

    friend constexpr bool operator==(const Rot2&, const Rot2&) = default;
};

/// `v` rotated by `r`, each component rounded once from its exact value.
[[nodiscard]] FixedVec2 rotate(Rot2 r, FixedVec2 v) noexcept;
/// `v` rotated by the inverse of `r`.
[[nodiscard]] FixedVec2 unrotate(Rot2 r, FixedVec2 v) noexcept;

// --- FixedQuat
// ------------------------------------------------------------------------------------

/// A rotation quaternion. `normalize()` goes through the wide square root and is applied after
/// every composition, so drift does not accumulate. Equality is raw equality.
struct FixedQuat {
    Fixed x;
    Fixed y;
    Fixed z;
    Fixed w = Fixed::one();

    [[nodiscard]] static constexpr FixedQuat identity() noexcept { return {}; }

    /// The rotation by `angle` about the unit vector `axis`, counter-clockwise seen from the axis's
    /// positive end. The half angle is `angle.raw >> 1` — exact — so `angle` and `angle` plus a
    /// whole turn give the same quaternion rather than its negation.
    [[nodiscard]] static FixedQuat from_axis_angle(FixedVec3 axis, Angle angle) noexcept;

    /// The inverse of a unit quaternion. Exact.
    [[nodiscard]] constexpr FixedQuat conjugate() const noexcept { return {-x, -y, -z, w}; }

    friend constexpr bool operator==(const FixedQuat&, const FixedQuat&) = default;
};

/// `q` scaled to unit length through the wide square root, each component truncated toward zero.
/// A zero quaternion gives the identity and is counted.
[[nodiscard]] FixedQuat normalize(FixedQuat q) noexcept;

/// `a * b`: `b` applied first. Each component of the Hamilton product is rounded once from its
/// exact value, then the result is normalised.
[[nodiscard]] FixedQuat operator*(FixedQuat a, FixedQuat b) noexcept;

/// `v` rotated by `q`, as `v + 2w (u x v) + 2 u x (u x v)` with `u` the vector part.
[[nodiscard]] FixedVec3 rotate(FixedQuat q, FixedVec3 v) noexcept;

// --- FixedTransform
// -------------------------------------------------------------------------------

/// A placement: a rotation, then a translation. No scale — authoritative simulation does not scale
/// units, and a project that needs one adds a `Fixed` uniform scale to its own component (design
/// §6).
struct FixedTransform {
    FixedVec3 translation;
    FixedQuat rotation;

    [[nodiscard]] static constexpr FixedTransform identity() noexcept { return {}; }

    /// The direction this transform faces: its rotation applied to -Z.
    [[nodiscard]] FixedVec3 forward() const noexcept;

    friend constexpr bool operator==(const FixedTransform&, const FixedTransform&) = default;
};

/// `p` placed by `t`: rotated, then translated.
[[nodiscard]] FixedVec3 transform_point(const FixedTransform& t, FixedVec3 p) noexcept;

/// `a * b`: `b` applied first, as core-math composes `Transform`.
[[nodiscard]] FixedTransform operator*(const FixedTransform& a, const FixedTransform& b) noexcept;

}  // namespace cy::detmath::inline CY_DETMATH_VARIANT
