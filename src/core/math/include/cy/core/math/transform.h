#pragma once
// Transform and Transform2D — the canonical representation of a placement in the world.
// Task 3.1.1.
//
// `core-math` — "Math types": **`Transform` is the canonical 3D transform, decomposed TRS rather
// than a matrix**, so interpolation, comparison and inversion are exact and cheap. Matrices are
// derived for rendering.
//
// The reason is the scenario the specification attaches to it: interpolating two `Transform`s lerps
// translation and scale and slerps rotation, with no decomposition and no shear artifacts. A matrix
// pair interpolated componentwise passes through non-rotations, and a matrix pair decomposed on
// every frame both costs more and is ambiguous — a matrix does not record whether a negative
// determinant came from a mirrored scale or from a rotation.
//
// The application order is **scale, then rotate, then translate**: `p' = t + R·(s ⊙ p)`.
// Composition `a * b` applies `b` first, matching `Mat4` and `Quat`, and tests/test_conventions.cpp
// asserts that
// `(a * b).to_matrix() == a.to_matrix() * b.to_matrix()` so the two representations cannot drift.

#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/scalar.h>
#include <cy/core/math/vec.h>

namespace cy {

/// A rotation, a translation and a non-uniform scale. Default-constructs to the identity, so a
/// zero-initialised component is a valid placement at the origin rather than a degenerate one.
struct Transform {
    Quat rotation = Quat::identity();
    Vec3 translation{0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f, 1.0f, 1.0f};

    [[nodiscard]] static constexpr Transform identity() noexcept { return Transform{}; }

    [[nodiscard]] static constexpr Transform from_translation(Vec3 t) noexcept {
        return Transform{Quat::identity(), t, Vec3{1.0f, 1.0f, 1.0f}};
    }

    [[nodiscard]] static constexpr Transform from_rotation(const Quat& r) noexcept {
        return Transform{r, Vec3{}, Vec3{1.0f, 1.0f, 1.0f}};
    }

    [[nodiscard]] static constexpr Transform from_scale(Vec3 s) noexcept {
        return Transform{Quat::identity(), Vec3{}, s};
    }

    /// The rendering form. Derived, never stored: a `Mat4` field beside a `Transform` is two
    /// sources of truth for one placement.
    [[nodiscard]] Mat4 to_matrix() const noexcept {
        return Mat4::from_trs(translation, rotation, scale);
    }

    /// The local axes in world space — the columns of the rotation, scaled.
    ///
    /// `forward()` is the local −Z, which is the whole of "−Z is forward" as a method. A camera
    /// looks down it and a character walks along it.
    [[nodiscard]] Vec3 forward() const noexcept { return rotation * kAxisForward; }
    [[nodiscard]] Vec3 up() const noexcept { return rotation * kAxisUp; }
    [[nodiscard]] Vec3 right() const noexcept { return rotation * kAxisRight; }

    /// A position through this transform: scale, then rotate, then translate.
    [[nodiscard]] Vec3 transform_point(Vec3 p) const noexcept {
        return translation + rotation * cwise_mul(scale, p);
    }

    /// A direction: rotation and scale apply, translation does not.
    [[nodiscard]] Vec3 transform_vector(Vec3 v) const noexcept {
        return rotation * cwise_mul(scale, v);
    }

    /// A direction with scale ignored as well — the right operation for an axis or a normalised
    /// heading, where a non-uniform scale would change the direction rather than its length.
    [[nodiscard]] Vec3 rotate_vector(Vec3 v) const noexcept { return rotation * v; }

    [[nodiscard]] bool has_uniform_scale(f32 tolerance = math::kEpsilon) const noexcept {
        return math::nearly_equal(scale.x, scale.y, tolerance) &&
               math::nearly_equal(scale.y, scale.z, tolerance);
    }
};

static_assert(sizeof(Transform) == 40);

[[nodiscard]] constexpr bool operator==(const Transform& a, const Transform& b) noexcept {
    return a.rotation == b.rotation && a.translation == b.translation && a.scale == b.scale;
}
[[nodiscard]] constexpr bool operator!=(const Transform& a, const Transform& b) noexcept {
    return !(a == b);
}

/// Composition: `a * b` applies `b` first, then `a`. Reads as "a's frame applied to b", which is
/// what a parent-child chain means when written `world = parent * local`.
///
/// The scale of the result is the componentwise product, which is exact only when `a`'s rotation
/// maps `b`'s scale axes onto its own — the general product of two non-uniformly scaled frames is
/// a shear and is not a TRS at all. That is a property of decomposed transforms, not a defect of
/// this function, and it is why non-uniform scale in a deep hierarchy is discouraged; a scene that
/// needs the exact answer composes matrices instead.
[[nodiscard]] Transform operator*(const Transform& a, const Transform& b) noexcept;

/// The inverse placement.
///
/// **Exact for uniform scale, approximate otherwise.** The exact inverse of `p ↦ t + R·(s ⊙ p)` is
/// `p ↦ (1/s) ⊙ (R⁻¹·(p − t))`, which applies the inverse scale *after* the rotation and so is not
/// of the form this type stores. Where the exact answer matters under non-uniform scale, invert the
/// matrix: `inverse(t.to_matrix())`.
[[nodiscard]] Transform inverse(const Transform& t) noexcept;

/// Translation and scale lerped, rotation slerped. This is `core-math`'s "TRS interpolation"
/// scenario, and it is the reason `Transform` is canonical.
[[nodiscard]] Transform interpolate(const Transform& a, const Transform& b, f32 t) noexcept;

/// Recover a TRS from an affine matrix, for an importer or an editor that received one.
///
/// Reports `ErrorCode::InvalidArgument` for a matrix that is not affine or whose linear part is
/// singular. Shear is not representable and is silently dropped — the returned transform is the
/// closest TRS, not an equal one — which is precisely why the engine passes `Transform` around
/// rather than `Mat4`.
[[nodiscard]] Expected<Transform, Error> decompose(const Mat4& m) noexcept;

[[nodiscard]] inline bool nearly_equal(const Transform& a, const Transform& b,
                                       f32 tolerance = math::kEpsilon) noexcept {
    return same_rotation(a.rotation, b.rotation, tolerance) &&
           nearly_equal(a.translation, b.translation, tolerance) &&
           nearly_equal(a.scale, b.scale, tolerance);
}

// --- 2D
// ---------------------------------------------------------------------------------------------

/// The 2D placement, for UI, sprites and 2D physics. Screen space has its origin at the top-left of
/// a viewport with +Y downward (`core-math` — "Coordinate conventions"), so a positive `rotation`
/// here reads as clockwise on screen even though it is counter-clockwise in the mathematics.
struct Transform2D {
    Vec2 translation{0.0f, 0.0f};
    Vec2 scale{1.0f, 1.0f};
    f32 rotation_radians = 0.0f;

    [[nodiscard]] static constexpr Transform2D identity() noexcept { return Transform2D{}; }

    [[nodiscard]] Vec2 transform_point(Vec2 p) const noexcept;
    [[nodiscard]] Vec2 transform_vector(Vec2 v) const noexcept;

    /// The 3x3 form, for a UI batch's uniform block.
    [[nodiscard]] Mat3 to_matrix() const noexcept;
};

static_assert(sizeof(Transform2D) == 20);

[[nodiscard]] Transform2D operator*(const Transform2D& a, const Transform2D& b) noexcept;
[[nodiscard]] Transform2D inverse(const Transform2D& t) noexcept;
[[nodiscard]] Transform2D interpolate(const Transform2D& a, const Transform2D& b, f32 t) noexcept;

}  // namespace cy
