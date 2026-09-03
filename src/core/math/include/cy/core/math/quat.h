#pragma once
// Quat — the engine's runtime rotation. Task 3.1.1.
//
// `core-math` — "Coordinate conventions": rotations are counter-clockwise about the axis when
// viewed from the positive axis toward the origin (the right-hand rule), and the default Euler
// order is **YXZ** — yaw about +Y, then pitch about +X, then roll about +Z. Euler angles appear
// only at authoring and interchange boundaries, which is why `to_euler_yxz` exists but nothing in
// the engine stores its result: `Transform` holds a quaternion.
//
// The convention that a quaternion rotates a vector as q·v·q⁻¹ is not negotiable, and the identity
// `cross(kAxisX, kAxisY) == kAxisZ` from vec.h is what fixes the sign of everything here.

#include <cy/core/base/assert.h>
#include <cy/core/base/types.h>
#include <cy/core/math/scalar.h>
#include <cy/core/math/vec.h>

#include <cmath>

namespace cy {

/// A rotation, stored as (x, y, z, w) with w last so that the memory layout matches the
/// x,y,z,w order every graphics API, glTF file and shader uses. The default value is the identity
/// rotation, so a zero-initialised `Transform` is the identity rather than a degenerate quaternion.
struct Quat {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 w = 1.0f;

    [[nodiscard]] static constexpr Quat identity() noexcept { return Quat{}; }

    /// A rotation of `angle_radians` about a **unit-length** `axis`, counter-clockwise when viewed
    /// from the positive end of the axis toward the origin.
    [[nodiscard]] static Quat from_axis_angle(Vec3 axis, f32 angle_radians) noexcept {
        CY_ASSERT_MSG(math::nearly_equal(length_squared(axis), 1.0f, 1e-3f),
                      "Quat::from_axis_angle() requires a unit-length axis");
        const f32 half = angle_radians * 0.5f;
        const f32 s = std::sin(half);
        return Quat{axis.x * s, axis.y * s, axis.z * s, std::cos(half)};
    }

    /// The engine's default Euler order, YXZ: yaw about +Y is applied first, then pitch about +X,
    /// then roll about +Z, so the composed rotation is `Ry * Rx * Rz`.
    ///
    /// `euler_radians` is (pitch, yaw, roll) — the components sit on the axes they rotate about,
    /// so `euler.y` is the yaw whatever the application order is. Naming them by axis rather than
    /// by position is what stops the order and the storage from being confused for each other.
    ///
    /// Defined out of line: the body composes quaternions, and `operator*` is declared after this
    /// class, where an inline member body could not see it.
    [[nodiscard]] static Quat from_euler_yxz(Vec3 euler_radians) noexcept;

    /// The rotation whose local −Z is `forward` and whose local +Y is as close to `up` as the
    /// constraint allows. `forward` and `up` need not be unit-length and need not be orthogonal;
    /// they must not be parallel.
    ///
    /// This is where "−Z is forward" becomes arithmetic: the basis is built with the local Z axis
    /// pointing *backwards*, at −`forward`. A look-at down −Z therefore produces the identity,
    /// which is the assertion in tests/test_conventions.cpp.
    [[nodiscard]] static Quat look_rotation(Vec3 forward, Vec3 up = kAxisUp) noexcept;

    /// Build from an orthonormal right-handed basis given as its three column vectors — the images
    /// of +X, +Y and +Z under the rotation.
    [[nodiscard]] static Quat from_basis(Vec3 x_axis, Vec3 y_axis, Vec3 z_axis) noexcept;

    /// The shortest rotation taking unit-length `from` onto unit-length `to`.
    [[nodiscard]] static Quat from_to(Vec3 from, Vec3 to) noexcept;

    [[nodiscard]] constexpr f32 operator[](usize i) const noexcept { return (&x)[i]; }
    [[nodiscard]] constexpr f32& operator[](usize i) noexcept { return (&x)[i]; }

    /// Yaw, pitch and roll recovered under the YXZ order, on the axes they rotate about. For an
    /// editor field or a glTF export, never for storage.
    [[nodiscard]] Vec3 to_euler_yxz() const noexcept;

    /// The axis this rotation is about, and how far around it. The axis is arbitrary but
    /// unit-length when the angle is zero.
    void to_axis_angle(Vec3& axis, f32& angle_radians) const noexcept;
};

static_assert(sizeof(Quat) == 16);

[[nodiscard]] constexpr bool operator==(const Quat& a, const Quat& b) noexcept {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}
[[nodiscard]] constexpr bool operator!=(const Quat& a, const Quat& b) noexcept {
    return !(a == b);
}

[[nodiscard]] constexpr Quat operator-(const Quat& q) noexcept {
    return Quat{-q.x, -q.y, -q.z, -q.w};
}

[[nodiscard]] constexpr Quat operator*(const Quat& q, f32 s) noexcept {
    return Quat{q.x * s, q.y * s, q.z * s, q.w * s};
}

[[nodiscard]] constexpr Quat operator+(const Quat& a, const Quat& b) noexcept {
    return Quat{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}

[[nodiscard]] constexpr Quat operator-(const Quat& a, const Quat& b) noexcept {
    return Quat{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}

/// Composition. `a * b` is "b first, then a" — the same reading order as matrix composition
/// (matrix.h) and as `Transform` composition, so the three cannot disagree.
[[nodiscard]] constexpr Quat operator*(const Quat& a, const Quat& b) noexcept {
    return Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

[[nodiscard]] constexpr f32 dot(const Quat& a, const Quat& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

[[nodiscard]] constexpr f32 length_squared(const Quat& q) noexcept {
    return dot(q, q);
}
[[nodiscard]] inline f32 length(const Quat& q) noexcept {
    return std::sqrt(dot(q, q));
}

[[nodiscard]] constexpr Quat conjugate(const Quat& q) noexcept {
    return Quat{-q.x, -q.y, -q.z, q.w};
}

/// The inverse rotation. For a unit-length quaternion this is the conjugate; the general form is
/// spelled out because a quaternion that has drifted off the unit sphere through repeated
/// composition is a real occurrence, and returning the conjugate for it would scale as well as
/// rotate.
[[nodiscard]] inline Quat inverse(const Quat& q) noexcept {
    const f32 len_sq = length_squared(q);
    CY_ASSERT_MSG(len_sq > math::kSmallLength, "inverse() on a zero-length Quat");
    return conjugate(q) * (1.0f / len_sq);
}

[[nodiscard]] inline Quat normalize(const Quat& q) noexcept {
    const f32 len_sq = length_squared(q);
    CY_ASSERT_MSG(len_sq > math::kSmallLength, "normalize() on a zero-length Quat");
    return q * math::rsqrt(len_sq);
}

/// Rotate a vector: q·v·q⁻¹, written in the expanded form that avoids constructing two
/// intermediate quaternions.
[[nodiscard]] constexpr Vec3 operator*(const Quat& q, Vec3 v) noexcept {
    const Vec3 qv{q.x, q.y, q.z};
    const Vec3 t = cross(qv, v) * 2.0f;
    return v + t * q.w + cross(qv, t);
}

/// Normalised linear interpolation: cheap, and not constant-angular-velocity. Correct for small
/// angles and for cases where the path does not matter.
[[nodiscard]] inline Quat nlerp(const Quat& a, const Quat& b, f32 t) noexcept {
    // Take the shorter of the two arcs. q and -q are the same rotation, so without this a pair
    // that happens to be stored with opposite signs interpolates the long way round.
    const Quat target = dot(a, b) < 0.0f ? -b : b;
    return normalize(a + (target - a) * t);
}

/// Spherical linear interpolation: constant angular velocity, which is what animation blending and
/// `Transform` interpolation require.
[[nodiscard]] Quat slerp(const Quat& a, const Quat& b, f32 t) noexcept;

/// The angle, in radians, of the rotation that takes `a` to `b`. Always in [0, pi].
[[nodiscard]] f32 angle_between(const Quat& a, const Quat& b) noexcept;

[[nodiscard]] inline bool nearly_equal(const Quat& a, const Quat& b,
                                       f32 tolerance = math::kEpsilon) noexcept {
    return math::nearly_equal(a.x, b.x, tolerance) && math::nearly_equal(a.y, b.y, tolerance) &&
           math::nearly_equal(a.z, b.z, tolerance) && math::nearly_equal(a.w, b.w, tolerance);
}

/// Whether two quaternions denote the same rotation, accounting for the double cover: `q` and `-q`
/// rotate identically, so a comparison that does not allow for it reports a false difference after
/// any operation that happens to flip the sign.
[[nodiscard]] inline bool same_rotation(const Quat& a, const Quat& b,
                                        f32 tolerance = math::kEpsilon) noexcept {
    return nearly_equal(a, b, tolerance) || nearly_equal(a, -b, tolerance);
}

}  // namespace cy
