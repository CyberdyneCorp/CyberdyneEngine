#pragma once
// Mat3 and Mat4. Task 3.1.1.
//
// `core-math` — "Math types": matrices are **column-major in memory** and use the **column-vector
// convention**, `M * v`, matching GLSL and the SPIR-V pipeline so that nothing is transposed on
// upload. Both halves of that sentence are load-bearing and they are independent of each other:
//
//   * Column-major *storage* means `columns[c]` is a contiguous `Vec4`, and the sixteen floats a
//     `Mat4` writes into a uniform buffer are already in the order a GLSL `mat4` expects.
//   * The column-vector *convention* means a point is a column and `M * v` transforms it, so
//     `A * B` applies B first and then A. Composition therefore reads right to left, which is the
//     same direction as `Quat` composition and `Transform` composition.
//
// tests/test_conventions.cpp asserts both: that `at(row, col)` finds the translation of a
// translation matrix in the fourth *column*, and that `A * B` applies B first.
//
// Element access is `at(row, col)`, spelled row-first because that is how the mathematics is
// written, over storage that is column-first. `columns[c][r]` is the same element and is what the
// implementations use; the two orders are the single most common source of transposed-matrix bugs,
// so the accessor names which one it takes.

#include <cy/core/base/assert.h>
#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/scalar.h>
#include <cy/core/math/vec.h>

namespace cy {

/// A 3x3 matrix: a rotation, a scale, a shear, or a normal matrix. Not a transform — a transform
/// has a translation, and this is the linear part on its own.
struct Mat3 {
    Vec3 columns[3] = {Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};

    [[nodiscard]] static constexpr Mat3 identity() noexcept { return Mat3{}; }
    [[nodiscard]] static constexpr Mat3 zero() noexcept { return Mat3{{Vec3{}, Vec3{}, Vec3{}}}; }

    [[nodiscard]] static constexpr Mat3 from_columns(Vec3 c0, Vec3 c1, Vec3 c2) noexcept {
        return Mat3{{c0, c1, c2}};
    }

    [[nodiscard]] static constexpr Mat3 from_scale(Vec3 s) noexcept {
        return Mat3{{Vec3{s.x, 0.0f, 0.0f}, Vec3{0.0f, s.y, 0.0f}, Vec3{0.0f, 0.0f, s.z}}};
    }

    [[nodiscard]] static Mat3 from_quat(const Quat& q) noexcept;

    /// Row-first access over column-first storage: `at(r, c) == columns[c][r]`.
    [[nodiscard]] constexpr f32 at(usize row, usize col) const noexcept {
        return columns[col][row];
    }
    [[nodiscard]] constexpr f32& at(usize row, usize col) noexcept { return columns[col][row]; }

    [[nodiscard]] constexpr Vec3 row(usize r) const noexcept {
        return Vec3{columns[0][r], columns[1][r], columns[2][r]};
    }

    /// The nine floats, column-major. The pointer is to the first element of the first column, and
    /// the columns are contiguous, which is what makes an upload a memcpy.
    [[nodiscard]] const f32* data() const noexcept { return &columns[0].x; }
};

static_assert(sizeof(Mat3) == 36, "Mat3 is nine tightly packed floats, uploadable without padding");

/// A 4x4 matrix: the derived rendering form of a transform, and the only form a projection takes.
///
/// `Transform` is the canonical representation of a 3D transform in the engine (`core-math` —
/// "Math types"); a `Mat4` is what a `Transform` becomes on its way to a shader. Storing a `Mat4`
/// where a `Transform` belongs loses the exact interpolation and inversion that decomposed TRS
/// gives, which is the reason the specification names one of them canonical.
struct Mat4 {
    Vec4 columns[4] = {Vec4{1.0f, 0.0f, 0.0f, 0.0f}, Vec4{0.0f, 1.0f, 0.0f, 0.0f},
                       Vec4{0.0f, 0.0f, 1.0f, 0.0f}, Vec4{0.0f, 0.0f, 0.0f, 1.0f}};

    [[nodiscard]] static constexpr Mat4 identity() noexcept { return Mat4{}; }
    [[nodiscard]] static constexpr Mat4 zero() noexcept {
        return Mat4{{Vec4{}, Vec4{}, Vec4{}, Vec4{}}};
    }

    [[nodiscard]] static constexpr Mat4 from_columns(Vec4 c0, Vec4 c1, Vec4 c2, Vec4 c3) noexcept {
        return Mat4{{c0, c1, c2, c3}};
    }

    /// Translation lives in the **fourth column** — `at(0..2, 3)` — which is the direct consequence
    /// of the column-vector convention. In a row-vector engine it would be the fourth row, and a
    /// matrix copied from one to the other without transposing is the classic silent corruption.
    [[nodiscard]] static constexpr Mat4 from_translation(Vec3 t) noexcept {
        return Mat4{{Vec4{1.0f, 0.0f, 0.0f, 0.0f}, Vec4{0.0f, 1.0f, 0.0f, 0.0f},
                     Vec4{0.0f, 0.0f, 1.0f, 0.0f}, Vec4{t.x, t.y, t.z, 1.0f}}};
    }

    [[nodiscard]] static constexpr Mat4 from_scale(Vec3 s) noexcept {
        return Mat4{{Vec4{s.x, 0.0f, 0.0f, 0.0f}, Vec4{0.0f, s.y, 0.0f, 0.0f},
                     Vec4{0.0f, 0.0f, s.z, 0.0f}, Vec4{0.0f, 0.0f, 0.0f, 1.0f}}};
    }

    [[nodiscard]] static Mat4 from_quat(const Quat& q) noexcept;

    [[nodiscard]] static constexpr Mat4 from_mat3(const Mat3& m) noexcept {
        return Mat4{{Vec4{m.columns[0].x, m.columns[0].y, m.columns[0].z, 0.0f},
                     Vec4{m.columns[1].x, m.columns[1].y, m.columns[1].z, 0.0f},
                     Vec4{m.columns[2].x, m.columns[2].y, m.columns[2].z, 0.0f},
                     Vec4{0.0f, 0.0f, 0.0f, 1.0f}}};
    }

    /// Scale, then rotate, then translate — the order a `Transform` means, built directly rather
    /// than by multiplying three matrices, because the product of the three is known in closed
    /// form and the direct construction cannot get the order wrong.
    [[nodiscard]] static Mat4 from_trs(Vec3 translation, const Quat& rotation, Vec3 scale) noexcept;

    [[nodiscard]] constexpr f32 at(usize row, usize col) const noexcept {
        return columns[col][row];
    }
    [[nodiscard]] constexpr f32& at(usize row, usize col) noexcept { return columns[col][row]; }

    [[nodiscard]] constexpr Vec4 row(usize r) const noexcept {
        return Vec4{columns[0][r], columns[1][r], columns[2][r], columns[3][r]};
    }

    /// The linear part: rotation and scale with the translation dropped.
    [[nodiscard]] constexpr Mat3 upper3x3() const noexcept {
        return Mat3{{columns[0].xyz(), columns[1].xyz(), columns[2].xyz()}};
    }

    [[nodiscard]] constexpr Vec3 translation() const noexcept { return columns[3].xyz(); }

    /// The sixteen floats, column-major, ready for a uniform buffer without a transpose.
    [[nodiscard]] const f32* data() const noexcept { return &columns[0].x; }
};

static_assert(sizeof(Mat4) == 64, "Mat4 is sixteen tightly packed floats, uploadable as-is");

// --- Mat3 operations
// --------------------------------------------------------------------------------

[[nodiscard]] constexpr bool operator==(const Mat3& a, const Mat3& b) noexcept {
    return a.columns[0] == b.columns[0] && a.columns[1] == b.columns[1] &&
           a.columns[2] == b.columns[2];
}
[[nodiscard]] constexpr bool operator!=(const Mat3& a, const Mat3& b) noexcept {
    return !(a == b);
}

[[nodiscard]] constexpr Vec3 operator*(const Mat3& m, Vec3 v) noexcept {
    return m.columns[0] * v.x + m.columns[1] * v.y + m.columns[2] * v.z;
}

[[nodiscard]] constexpr Mat3 operator*(const Mat3& a, const Mat3& b) noexcept {
    return Mat3{{a * b.columns[0], a * b.columns[1], a * b.columns[2]}};
}

[[nodiscard]] constexpr Mat3 transpose(const Mat3& m) noexcept {
    return Mat3{{m.row(0), m.row(1), m.row(2)}};
}

[[nodiscard]] constexpr f32 determinant(const Mat3& m) noexcept {
    return dot(m.columns[0], cross(m.columns[1], m.columns[2]));
}

/// The inverse, or `ErrorCode::InvalidArgument` when the matrix is singular. A singular matrix is
/// data, not a programmer error — a scale of zero on one axis is a legitimate thing for a scene to
/// contain — so this reports rather than asserts.
[[nodiscard]] Expected<Mat3, Error> inverse(const Mat3& m) noexcept;

/// The matrix that transforms normals under `m`: the inverse transpose of its linear part.
/// Falls back to `m` itself when `m` is singular, which is the conventional degenerate answer and
/// keeps a caller from having to branch in a shader-feeding path.
[[nodiscard]] Mat3 normal_matrix(const Mat3& m) noexcept;

/// The rotation `m` represents, assuming `m` is orthonormal. A matrix carrying scale or shear
/// gives a meaningless answer, which is why `Transform` keeps rotation and scale apart rather than
/// recovering them from a matrix.
[[nodiscard]] Quat to_quat(const Mat3& m) noexcept;

// --- Mat4 operations
// --------------------------------------------------------------------------------

[[nodiscard]] constexpr bool operator==(const Mat4& a, const Mat4& b) noexcept {
    return a.columns[0] == b.columns[0] && a.columns[1] == b.columns[1] &&
           a.columns[2] == b.columns[2] && a.columns[3] == b.columns[3];
}
[[nodiscard]] constexpr bool operator!=(const Mat4& a, const Mat4& b) noexcept {
    return !(a == b);
}

[[nodiscard]] constexpr Vec4 operator*(const Mat4& m, Vec4 v) noexcept {
    return m.columns[0] * v.x + m.columns[1] * v.y + m.columns[2] * v.z + m.columns[3] * v.w;
}

/// Composition, right to left: `a * b` applies `b` first and then `a`. Reversing it is the single
/// most common convention error in an engine, so tests/test_conventions.cpp asserts the direction
/// against a translate-then-rotate case whose two orders give visibly different answers.
[[nodiscard]] constexpr Mat4 operator*(const Mat4& a, const Mat4& b) noexcept {
    return Mat4{{a * b.columns[0], a * b.columns[1], a * b.columns[2], a * b.columns[3]}};
}

[[nodiscard]] constexpr Mat4 transpose(const Mat4& m) noexcept {
    return Mat4{{m.row(0), m.row(1), m.row(2), m.row(3)}};
}

/// A position through an affine matrix: w = 1, and no perspective divide. Use `project_point` for
/// a projection matrix, where the divide is the point.
[[nodiscard]] constexpr Vec3 transform_point(const Mat4& m, Vec3 p) noexcept {
    return (m.columns[0] * p.x + m.columns[1] * p.y + m.columns[2] * p.z + m.columns[3]).xyz();
}

/// A direction: w = 0, so translation does not apply. Scale and rotation still do, so this is not
/// a normal transform — see `normal_matrix`.
[[nodiscard]] constexpr Vec3 transform_direction(const Mat4& m, Vec3 d) noexcept {
    return (m.columns[0] * d.x + m.columns[1] * d.y + m.columns[2] * d.z).xyz();
}

/// A position through a projection, with the perspective divide applied. `w` receives the
/// clip-space w, which is the view-space depth a caller usually wants next.
[[nodiscard]] Vec3 project_point(const Mat4& m, Vec3 p, f32* out_clip_w = nullptr) noexcept;

[[nodiscard]] f32 determinant(const Mat4& m) noexcept;

/// The general inverse. `ErrorCode::InvalidArgument` when the matrix is singular.
[[nodiscard]] Expected<Mat4, Error> inverse(const Mat4& m) noexcept;

/// The inverse of a matrix known to be affine — a rotation, a scale and a translation, with a
/// bottom row of (0, 0, 0, 1). Cheaper than the general inverse and numerically better behaved,
/// and the case almost every view matrix and every node transform actually is.
///
/// Asserts the bottom row in development builds. It reports rather than asserts on a singular
/// linear part, for the same reason `inverse(Mat3)` does.
[[nodiscard]] Expected<Mat4, Error> inverse_affine(const Mat4& m) noexcept;

[[nodiscard]] bool nearly_equal(const Mat4& a, const Mat4& b,
                                f32 tolerance = math::kEpsilon) noexcept;
[[nodiscard]] bool nearly_equal(const Mat3& a, const Mat3& b,
                                f32 tolerance = math::kEpsilon) noexcept;

}  // namespace cy
