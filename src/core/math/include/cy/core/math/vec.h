#pragma once
// Vec2, Vec3, Vec4 and their integer counterparts. Task 3.1.1.
//
// `core-math` — "SIMD strategy": **`Vec3` is 12 bytes and is never padded to 16.** Storage density
// wins because the engine stores far more positions than it multiplies at any one instant, and the
// SIMD paths operate on `Vec4` or on batched arrays (see batch.h) rather than on a single padded
// `Vec3`. The static_assert below is the enforcement; deleting it to "make SIMD easier" would
// silently grow every vertex, every component and every chunk in the engine by a third.
//
// Every type here is an aggregate: no constructors, no base classes, no virtuals. That is what
// makes them usable in `constexpr`, memcpy-able into GPU buffers, trivially relocatable in chunked
// storage, and describable by reflection without a special case.
//
// Componentwise multiplication is spelled `cwise_mul`, not `operator*`. `a * b` on two vectors
// reads as a product and every reader has a different product in mind; the ones that matter (dot,
// cross) have names.

#include <cy/core/base/assert.h>
#include <cy/core/base/types.h>
#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy {

// --- Types
// ----------------------------------------------------------------------------------------

struct Vec2 {
    f32 x = 0.0f;
    f32 y = 0.0f;

    [[nodiscard]] constexpr f32 operator[](usize i) const noexcept { return (&x)[i]; }
    [[nodiscard]] constexpr f32& operator[](usize i) noexcept { return (&x)[i]; }
};

struct Vec3 {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;

    [[nodiscard]] constexpr f32 operator[](usize i) const noexcept { return (&x)[i]; }
    [[nodiscard]] constexpr f32& operator[](usize i) noexcept { return (&x)[i]; }

    [[nodiscard]] constexpr Vec2 xy() const noexcept { return Vec2{x, y}; }
};

struct Vec4 {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 w = 0.0f;

    [[nodiscard]] constexpr f32 operator[](usize i) const noexcept { return (&x)[i]; }
    [[nodiscard]] constexpr f32& operator[](usize i) noexcept { return (&x)[i]; }

    [[nodiscard]] constexpr Vec3 xyz() const noexcept { return Vec3{x, y, z}; }
    [[nodiscard]] constexpr Vec2 xy() const noexcept { return Vec2{x, y}; }
};

struct IVec2 {
    i32 x = 0;
    i32 y = 0;

    [[nodiscard]] constexpr i32 operator[](usize i) const noexcept { return (&x)[i]; }
    [[nodiscard]] constexpr i32& operator[](usize i) noexcept { return (&x)[i]; }
};

struct IVec3 {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;

    [[nodiscard]] constexpr i32 operator[](usize i) const noexcept { return (&x)[i]; }
    [[nodiscard]] constexpr i32& operator[](usize i) noexcept { return (&x)[i]; }
};

struct IVec4 {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    i32 w = 0;

    [[nodiscard]] constexpr i32 operator[](usize i) const noexcept { return (&x)[i]; }
    [[nodiscard]] constexpr i32& operator[](usize i) noexcept { return (&x)[i]; }
};

// The density requirement, checked rather than commented. A `Vec3` that grew to 16 bytes would
// still compile everywhere and would cost a third of the memory bandwidth of every position stream
// in the engine, which is the kind of regression that is only ever found by profiling.
static_assert(sizeof(Vec3) == 12, "Vec3 must be three floats: core-math forbids padding it to 16");
static_assert(sizeof(Vec2) == 8);
static_assert(sizeof(Vec4) == 16);
static_assert(alignof(Vec4) == 4, "Vec4 is not over-aligned: arrays of it stay tightly packed");
static_assert(sizeof(IVec3) == 12);

// --- The engine's basis
// ---------------------------------------------------------------------------
//
// `core-math` — "Coordinate conventions": right-handed, Y-up, and an object's local **−Z is
// forward**. These three constants are the whole of that convention as data, and
// tests/test_conventions.cpp asserts the numeric consequences of it. Anything that needs "which
// way is forward" reads it here rather than writing a literal, so that a future change is one
// edit and a failing test rather than a search.

inline constexpr Vec3 kAxisRight{1.0f, 0.0f, 0.0f};
inline constexpr Vec3 kAxisUp{0.0f, 1.0f, 0.0f};
inline constexpr Vec3 kAxisForward{0.0f, 0.0f, -1.0f};
inline constexpr Vec3 kAxisX{1.0f, 0.0f, 0.0f};
inline constexpr Vec3 kAxisY{0.0f, 1.0f, 0.0f};
inline constexpr Vec3 kAxisZ{0.0f, 0.0f, 1.0f};

// --- Vec2
// -------------------------------------------------------------------------------------------

[[nodiscard]] constexpr Vec2 operator-(Vec2 v) noexcept {
    return Vec2{-v.x, -v.y};
}
[[nodiscard]] constexpr Vec2 operator+(Vec2 a, Vec2 b) noexcept {
    return Vec2{a.x + b.x, a.y + b.y};
}
[[nodiscard]] constexpr Vec2 operator-(Vec2 a, Vec2 b) noexcept {
    return Vec2{a.x - b.x, a.y - b.y};
}
[[nodiscard]] constexpr Vec2 operator*(Vec2 v, f32 s) noexcept {
    return Vec2{v.x * s, v.y * s};
}
[[nodiscard]] constexpr Vec2 operator*(f32 s, Vec2 v) noexcept {
    return v * s;
}
[[nodiscard]] constexpr Vec2 operator/(Vec2 v, f32 s) noexcept {
    return Vec2{v.x / s, v.y / s};
}

constexpr Vec2& operator+=(Vec2& a, Vec2 b) noexcept {
    return a = a + b;
}
constexpr Vec2& operator-=(Vec2& a, Vec2 b) noexcept {
    return a = a - b;
}
constexpr Vec2& operator*=(Vec2& a, f32 s) noexcept {
    return a = a * s;
}
constexpr Vec2& operator/=(Vec2& a, f32 s) noexcept {
    return a = a / s;
}

[[nodiscard]] constexpr bool operator==(Vec2 a, Vec2 b) noexcept {
    return a.x == b.x && a.y == b.y;
}
[[nodiscard]] constexpr bool operator!=(Vec2 a, Vec2 b) noexcept {
    return !(a == b);
}

[[nodiscard]] constexpr f32 dot(Vec2 a, Vec2 b) noexcept {
    return a.x * b.x + a.y * b.y;
}

/// The 2D "cross product": the z component of the 3D cross of the two vectors lifted into the
/// plane. Positive when `b` is counter-clockwise from `a`. It is the signed area of the
/// parallelogram, which is what the orientation and triangulation code in geometry.h actually asks
/// for.
[[nodiscard]] constexpr f32 cross(Vec2 a, Vec2 b) noexcept {
    return a.x * b.y - a.y * b.x;
}

[[nodiscard]] constexpr Vec2 cwise_mul(Vec2 a, Vec2 b) noexcept {
    return Vec2{a.x * b.x, a.y * b.y};
}
[[nodiscard]] constexpr Vec2 cwise_div(Vec2 a, Vec2 b) noexcept {
    return Vec2{a.x / b.x, a.y / b.y};
}
[[nodiscard]] constexpr Vec2 cwise_min(Vec2 a, Vec2 b) noexcept {
    return Vec2{math::min(a.x, b.x), math::min(a.y, b.y)};
}
[[nodiscard]] constexpr Vec2 cwise_max(Vec2 a, Vec2 b) noexcept {
    return Vec2{math::max(a.x, b.x), math::max(a.y, b.y)};
}

[[nodiscard]] constexpr f32 length_squared(Vec2 v) noexcept {
    return dot(v, v);
}
[[nodiscard]] inline f32 length(Vec2 v) noexcept {
    return std::sqrt(dot(v, v));
}
[[nodiscard]] inline f32 distance(Vec2 a, Vec2 b) noexcept {
    return length(b - a);
}
[[nodiscard]] constexpr f32 distance_squared(Vec2 a, Vec2 b) noexcept {
    return length_squared(b - a);
}
[[nodiscard]] constexpr Vec2 lerp(Vec2 a, Vec2 b, f32 t) noexcept {
    return a + (b - a) * t;
}

/// Unit-length `v`. Normalising a zero-length vector is a programmer error, not a runtime failure:
/// there is no direction to return and every answer is a lie. Use `normalized_or` where a zero is
/// a legitimate input.
[[nodiscard]] inline Vec2 normalize(Vec2 v) noexcept {
    const f32 len_sq = length_squared(v);
    CY_ASSERT_MSG(len_sq > math::kSmallLength, "normalize() on a zero-length Vec2");
    return v * math::rsqrt(len_sq);
}

[[nodiscard]] inline Vec2 normalized_or(Vec2 v, Vec2 fallback) noexcept {
    const f32 len_sq = length_squared(v);
    return len_sq > math::kSmallLength ? v * math::rsqrt(len_sq) : fallback;
}

/// Rotated 90 degrees counter-clockwise in the mathematical plane. Note that 2D *screen* space has
/// +Y downward (`core-math` — "Coordinate conventions"), so on screen this reads as clockwise.
[[nodiscard]] constexpr Vec2 perpendicular(Vec2 v) noexcept {
    return Vec2{-v.y, v.x};
}

[[nodiscard]] inline bool nearly_equal(Vec2 a, Vec2 b, f32 tolerance = math::kEpsilon) noexcept {
    return math::nearly_equal(a.x, b.x, tolerance) && math::nearly_equal(a.y, b.y, tolerance);
}

// --- Vec3
// -------------------------------------------------------------------------------------------

[[nodiscard]] constexpr Vec3 operator-(Vec3 v) noexcept {
    return Vec3{-v.x, -v.y, -v.z};
}
[[nodiscard]] constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}
[[nodiscard]] constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}
[[nodiscard]] constexpr Vec3 operator*(Vec3 v, f32 s) noexcept {
    return Vec3{v.x * s, v.y * s, v.z * s};
}
[[nodiscard]] constexpr Vec3 operator*(f32 s, Vec3 v) noexcept {
    return v * s;
}
[[nodiscard]] constexpr Vec3 operator/(Vec3 v, f32 s) noexcept {
    return Vec3{v.x / s, v.y / s, v.z / s};
}

constexpr Vec3& operator+=(Vec3& a, Vec3 b) noexcept {
    return a = a + b;
}
constexpr Vec3& operator-=(Vec3& a, Vec3 b) noexcept {
    return a = a - b;
}
constexpr Vec3& operator*=(Vec3& a, f32 s) noexcept {
    return a = a * s;
}
constexpr Vec3& operator/=(Vec3& a, f32 s) noexcept {
    return a = a / s;
}

[[nodiscard]] constexpr bool operator==(Vec3 a, Vec3 b) noexcept {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
[[nodiscard]] constexpr bool operator!=(Vec3 a, Vec3 b) noexcept {
    return !(a == b);
}

[[nodiscard]] constexpr f32 dot(Vec3 a, Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

/// Right-handed cross product: `cross(kAxisX, kAxisY) == kAxisZ`. That identity is the handedness
/// of the whole engine expressed in one line, and tests/test_conventions.cpp asserts it.
[[nodiscard]] constexpr Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

[[nodiscard]] constexpr Vec3 cwise_mul(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.x * b.x, a.y * b.y, a.z * b.z};
}
[[nodiscard]] constexpr Vec3 cwise_div(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.x / b.x, a.y / b.y, a.z / b.z};
}
[[nodiscard]] constexpr Vec3 cwise_min(Vec3 a, Vec3 b) noexcept {
    return Vec3{math::min(a.x, b.x), math::min(a.y, b.y), math::min(a.z, b.z)};
}
[[nodiscard]] constexpr Vec3 cwise_max(Vec3 a, Vec3 b) noexcept {
    return Vec3{math::max(a.x, b.x), math::max(a.y, b.y), math::max(a.z, b.z)};
}
[[nodiscard]] inline Vec3 cwise_abs(Vec3 v) noexcept {
    return Vec3{std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)};
}
[[nodiscard]] constexpr f32 max_component(Vec3 v) noexcept {
    return math::max(v.x, math::max(v.y, v.z));
}
[[nodiscard]] constexpr f32 min_component(Vec3 v) noexcept {
    return math::min(v.x, math::min(v.y, v.z));
}

[[nodiscard]] constexpr f32 length_squared(Vec3 v) noexcept {
    return dot(v, v);
}
[[nodiscard]] inline f32 length(Vec3 v) noexcept {
    return std::sqrt(dot(v, v));
}
[[nodiscard]] inline f32 distance(Vec3 a, Vec3 b) noexcept {
    return length(b - a);
}
[[nodiscard]] constexpr f32 distance_squared(Vec3 a, Vec3 b) noexcept {
    return length_squared(b - a);
}
[[nodiscard]] constexpr Vec3 lerp(Vec3 a, Vec3 b, f32 t) noexcept {
    return a + (b - a) * t;
}

[[nodiscard]] inline Vec3 normalize(Vec3 v) noexcept {
    const f32 len_sq = length_squared(v);
    CY_ASSERT_MSG(len_sq > math::kSmallLength, "normalize() on a zero-length Vec3");
    return v * math::rsqrt(len_sq);
}

[[nodiscard]] inline Vec3 normalized_or(Vec3 v, Vec3 fallback) noexcept {
    const f32 len_sq = length_squared(v);
    return len_sq > math::kSmallLength ? v * math::rsqrt(len_sq) : fallback;
}

/// `v` reflected about a unit-length `normal`, in the sense a mirror reflects: the component along
/// the normal is negated. An incident ray travelling *into* a surface reflects back out.
[[nodiscard]] constexpr Vec3 reflect(Vec3 v, Vec3 normal) noexcept {
    return v - normal * (2.0f * dot(v, normal));
}

/// The part of `v` that lies along a unit-length `direction`.
[[nodiscard]] constexpr Vec3 project_onto_unit(Vec3 v, Vec3 direction) noexcept {
    return direction * dot(v, direction);
}

/// The part of `v` perpendicular to a unit-length `direction`.
[[nodiscard]] constexpr Vec3 reject_from_unit(Vec3 v, Vec3 direction) noexcept {
    return v - project_onto_unit(v, direction);
}

/// Any unit vector perpendicular to a unit-length `v`. Used to complete a basis where only one
/// axis is constrained — a tangent frame, a disk sample, a debug arrow's crossbar.
///
/// The branch picks the axis `v` is *least* aligned with, so the cross product is never near-zero
/// and the result never degenerates.
[[nodiscard]] inline Vec3 any_perpendicular(Vec3 v) noexcept {
    const Vec3 axis = std::fabs(v.x) < 0.9f ? kAxisX : kAxisY;
    return normalize(cross(axis, v));
}

[[nodiscard]] inline bool nearly_equal(Vec3 a, Vec3 b, f32 tolerance = math::kEpsilon) noexcept {
    return math::nearly_equal(a.x, b.x, tolerance) && math::nearly_equal(a.y, b.y, tolerance) &&
           math::nearly_equal(a.z, b.z, tolerance);
}

[[nodiscard]] inline bool is_finite(Vec3 v) noexcept {
    return math::is_finite(v.x) && math::is_finite(v.y) && math::is_finite(v.z);
}

// --- Vec4
// -------------------------------------------------------------------------------------------

[[nodiscard]] constexpr Vec4 operator-(Vec4 v) noexcept {
    return Vec4{-v.x, -v.y, -v.z, -v.w};
}
[[nodiscard]] constexpr Vec4 operator+(Vec4 a, Vec4 b) noexcept {
    return Vec4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
[[nodiscard]] constexpr Vec4 operator-(Vec4 a, Vec4 b) noexcept {
    return Vec4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}
[[nodiscard]] constexpr Vec4 operator*(Vec4 v, f32 s) noexcept {
    return Vec4{v.x * s, v.y * s, v.z * s, v.w * s};
}
[[nodiscard]] constexpr Vec4 operator*(f32 s, Vec4 v) noexcept {
    return v * s;
}
[[nodiscard]] constexpr Vec4 operator/(Vec4 v, f32 s) noexcept {
    return Vec4{v.x / s, v.y / s, v.z / s, v.w / s};
}

constexpr Vec4& operator+=(Vec4& a, Vec4 b) noexcept {
    return a = a + b;
}
constexpr Vec4& operator-=(Vec4& a, Vec4 b) noexcept {
    return a = a - b;
}
constexpr Vec4& operator*=(Vec4& a, f32 s) noexcept {
    return a = a * s;
}
constexpr Vec4& operator/=(Vec4& a, f32 s) noexcept {
    return a = a / s;
}

[[nodiscard]] constexpr bool operator==(Vec4 a, Vec4 b) noexcept {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}
[[nodiscard]] constexpr bool operator!=(Vec4 a, Vec4 b) noexcept {
    return !(a == b);
}

[[nodiscard]] constexpr f32 dot(Vec4 a, Vec4 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}
[[nodiscard]] constexpr f32 length_squared(Vec4 v) noexcept {
    return dot(v, v);
}
[[nodiscard]] inline f32 length(Vec4 v) noexcept {
    return std::sqrt(dot(v, v));
}
[[nodiscard]] constexpr Vec4 lerp(Vec4 a, Vec4 b, f32 t) noexcept {
    return a + (b - a) * t;
}
[[nodiscard]] constexpr Vec4 cwise_mul(Vec4 a, Vec4 b) noexcept {
    return Vec4{a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}

[[nodiscard]] inline Vec4 normalize(Vec4 v) noexcept {
    const f32 len_sq = length_squared(v);
    CY_ASSERT_MSG(len_sq > math::kSmallLength, "normalize() on a zero-length Vec4");
    return v * math::rsqrt(len_sq);
}

/// A position lifted to homogeneous coordinates: w = 1, so a translation applies to it.
[[nodiscard]] constexpr Vec4 point4(Vec3 v) noexcept {
    return Vec4{v.x, v.y, v.z, 1.0f};
}

/// A direction lifted to homogeneous coordinates: w = 0, so a translation does not.
[[nodiscard]] constexpr Vec4 direction4(Vec3 v) noexcept {
    return Vec4{v.x, v.y, v.z, 0.0f};
}

[[nodiscard]] inline bool nearly_equal(Vec4 a, Vec4 b, f32 tolerance = math::kEpsilon) noexcept {
    return math::nearly_equal(a.x, b.x, tolerance) && math::nearly_equal(a.y, b.y, tolerance) &&
           math::nearly_equal(a.z, b.z, tolerance) && math::nearly_equal(a.w, b.w, tolerance);
}

// --- Integer vectors
// ---------------------------------------------------------------------------------
//
// Enough to index a grid, size a viewport and address a cell. Integer vectors are used for
// discrete quantities, so there is no `length()` here: the length of a grid coordinate is a
// floating-point number and asking for it usually means the caller wanted a `Vec3`.

[[nodiscard]] constexpr IVec2 operator+(IVec2 a, IVec2 b) noexcept {
    return IVec2{a.x + b.x, a.y + b.y};
}
[[nodiscard]] constexpr IVec2 operator-(IVec2 a, IVec2 b) noexcept {
    return IVec2{a.x - b.x, a.y - b.y};
}
[[nodiscard]] constexpr IVec2 operator*(IVec2 v, i32 s) noexcept {
    return IVec2{v.x * s, v.y * s};
}
[[nodiscard]] constexpr bool operator==(IVec2 a, IVec2 b) noexcept {
    return a.x == b.x && a.y == b.y;
}
[[nodiscard]] constexpr bool operator!=(IVec2 a, IVec2 b) noexcept {
    return !(a == b);
}

[[nodiscard]] constexpr IVec3 operator+(IVec3 a, IVec3 b) noexcept {
    return IVec3{a.x + b.x, a.y + b.y, a.z + b.z};
}
[[nodiscard]] constexpr IVec3 operator-(IVec3 a, IVec3 b) noexcept {
    return IVec3{a.x - b.x, a.y - b.y, a.z - b.z};
}
[[nodiscard]] constexpr IVec3 operator*(IVec3 v, i32 s) noexcept {
    return IVec3{v.x * s, v.y * s, v.z * s};
}
[[nodiscard]] constexpr bool operator==(IVec3 a, IVec3 b) noexcept {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
[[nodiscard]] constexpr bool operator!=(IVec3 a, IVec3 b) noexcept {
    return !(a == b);
}

[[nodiscard]] constexpr IVec4 operator+(IVec4 a, IVec4 b) noexcept {
    return IVec4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
[[nodiscard]] constexpr IVec4 operator-(IVec4 a, IVec4 b) noexcept {
    return IVec4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}
[[nodiscard]] constexpr bool operator==(IVec4 a, IVec4 b) noexcept {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}
[[nodiscard]] constexpr bool operator!=(IVec4 a, IVec4 b) noexcept {
    return !(a == b);
}

[[nodiscard]] constexpr Vec2 to_vec2(IVec2 v) noexcept {
    return Vec2{static_cast<f32>(v.x), static_cast<f32>(v.y)};
}
[[nodiscard]] constexpr Vec3 to_vec3(IVec3 v) noexcept {
    return Vec3{static_cast<f32>(v.x), static_cast<f32>(v.y), static_cast<f32>(v.z)};
}

/// Floor-to-integer, which is what a grid lookup needs: -0.5 belongs to cell -1, not to cell 0.
/// `static_cast<i32>` truncates toward zero and would put it in cell 0, making the origin cell
/// twice as wide as every other one.
[[nodiscard]] inline IVec3 floor_to_ivec3(Vec3 v) noexcept {
    return IVec3{static_cast<i32>(std::floor(v.x)), static_cast<i32>(std::floor(v.y)),
                 static_cast<i32>(std::floor(v.z))};
}

}  // namespace cy
