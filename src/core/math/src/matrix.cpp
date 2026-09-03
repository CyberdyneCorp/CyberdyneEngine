// Mat3 and Mat4's out-of-line operations. Task 3.1.1. See include/cy/core/math/matrix.h.

#include <cy/core/math/matrix.h>

#include <cmath>

namespace cy {
namespace {

/// A determinant below this is treated as singular. Absolute rather than relative, and therefore
/// scale-dependent: a matrix built from millimetre-scale values has a genuinely tiny determinant
/// and is not singular. Callers working at such scales should test `determinant()` themselves — the
/// alternative, a relative test against the matrix norm, costs more than the inversion for the
/// benefit of a case the engine does not have (`core-math` — "Precision" puts the engine in
/// metres).
constexpr f32 kSingularDeterminant = 1e-20f;

}  // namespace

Mat3 Mat3::from_quat(const Quat& q) noexcept {
    const f32 xx = q.x * q.x;
    const f32 yy = q.y * q.y;
    const f32 zz = q.z * q.z;
    const f32 xy = q.x * q.y;
    const f32 xz = q.x * q.z;
    const f32 yz = q.y * q.z;
    const f32 wx = q.w * q.x;
    const f32 wy = q.w * q.y;
    const f32 wz = q.w * q.z;

    // Each column is the image of a basis vector, which is why the transposed-looking assignment is
    // correct: column 0 is where +X goes.
    return Mat3{{Vec3{1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy)},
                 Vec3{2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx)},
                 Vec3{2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy)}}};
}

Mat4 Mat4::from_quat(const Quat& q) noexcept {
    return Mat4::from_mat3(Mat3::from_quat(q));
}

Mat4 Mat4::from_trs(Vec3 translation_value, const Quat& rotation, Vec3 scale) noexcept {
    const Mat3 r = Mat3::from_quat(rotation);
    const Vec3 c0 = r.columns[0] * scale.x;
    const Vec3 c1 = r.columns[1] * scale.y;
    const Vec3 c2 = r.columns[2] * scale.z;
    return Mat4{{Vec4{c0.x, c0.y, c0.z, 0.0f}, Vec4{c1.x, c1.y, c1.z, 0.0f},
                 Vec4{c2.x, c2.y, c2.z, 0.0f},
                 Vec4{translation_value.x, translation_value.y, translation_value.z, 1.0f}}};
}

Expected<Mat3, Error> inverse(const Mat3& m) noexcept {
    // The adjugate over the determinant, written as three cross products: the rows of the adjugate
    // are the cross products of the columns, which is the cleanest form and the cheapest.
    const Vec3 r0 = cross(m.columns[1], m.columns[2]);
    const Vec3 r1 = cross(m.columns[2], m.columns[0]);
    const Vec3 r2 = cross(m.columns[0], m.columns[1]);
    const f32 det = dot(m.columns[0], r0);
    if (std::fabs(det) < kSingularDeterminant) {
        return cy::fail(ErrorCode::InvalidArgument, "inverse(Mat3): the matrix is singular");
    }
    const f32 inv_det = 1.0f / det;
    // r0, r1, r2 are the *rows* of the inverse, so they are transposed into columns here.
    return Mat3{{Vec3{r0.x, r1.x, r2.x} * inv_det, Vec3{r0.y, r1.y, r2.y} * inv_det,
                 Vec3{r0.z, r1.z, r2.z} * inv_det}};
}

Mat3 normal_matrix(const Mat3& m) noexcept {
    const Expected<Mat3, Error> inv = inverse(m);
    if (!inv) {
        return m;
    }
    return transpose(*inv);
}

Quat to_quat(const Mat3& m) noexcept {
    return Quat::from_basis(m.columns[0], m.columns[1], m.columns[2]);
}

Vec3 project_point(const Mat4& m, Vec3 p, f32* out_clip_w) noexcept {
    const Vec4 clip = m * point4(p);
    if (out_clip_w != nullptr) {
        *out_clip_w = clip.w;
    }
    if (clip.w == 0.0f) {
        // A point exactly on the camera plane has no projection. Returning the unnormalised clip
        // position keeps the caller from dividing by zero itself, and `out_clip_w` is how it finds
        // out — which is why the parameter exists.
        return clip.xyz();
    }
    return clip.xyz() * (1.0f / clip.w);
}

f32 determinant(const Mat4& m) noexcept {
    // Laplace expansion over the first two columns, sharing the six 2x2 minors between the four
    // 3x3 cofactors. Twice as fast as the naive expansion and no less readable once named.
    const f32 s0 = m.at(0, 0) * m.at(1, 1) - m.at(1, 0) * m.at(0, 1);
    const f32 s1 = m.at(0, 0) * m.at(1, 2) - m.at(1, 0) * m.at(0, 2);
    const f32 s2 = m.at(0, 0) * m.at(1, 3) - m.at(1, 0) * m.at(0, 3);
    const f32 s3 = m.at(0, 1) * m.at(1, 2) - m.at(1, 1) * m.at(0, 2);
    const f32 s4 = m.at(0, 1) * m.at(1, 3) - m.at(1, 1) * m.at(0, 3);
    const f32 s5 = m.at(0, 2) * m.at(1, 3) - m.at(1, 2) * m.at(0, 3);

    const f32 c5 = m.at(2, 2) * m.at(3, 3) - m.at(3, 2) * m.at(2, 3);
    const f32 c4 = m.at(2, 1) * m.at(3, 3) - m.at(3, 1) * m.at(2, 3);
    const f32 c3 = m.at(2, 1) * m.at(3, 2) - m.at(3, 1) * m.at(2, 2);
    const f32 c2 = m.at(2, 0) * m.at(3, 3) - m.at(3, 0) * m.at(2, 3);
    const f32 c1 = m.at(2, 0) * m.at(3, 2) - m.at(3, 0) * m.at(2, 2);
    const f32 c0 = m.at(2, 0) * m.at(3, 1) - m.at(3, 0) * m.at(2, 1);

    return s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
}

Expected<Mat4, Error> inverse(const Mat4& m) noexcept {
    const f32 s0 = m.at(0, 0) * m.at(1, 1) - m.at(1, 0) * m.at(0, 1);
    const f32 s1 = m.at(0, 0) * m.at(1, 2) - m.at(1, 0) * m.at(0, 2);
    const f32 s2 = m.at(0, 0) * m.at(1, 3) - m.at(1, 0) * m.at(0, 3);
    const f32 s3 = m.at(0, 1) * m.at(1, 2) - m.at(1, 1) * m.at(0, 2);
    const f32 s4 = m.at(0, 1) * m.at(1, 3) - m.at(1, 1) * m.at(0, 3);
    const f32 s5 = m.at(0, 2) * m.at(1, 3) - m.at(1, 2) * m.at(0, 3);

    const f32 c5 = m.at(2, 2) * m.at(3, 3) - m.at(3, 2) * m.at(2, 3);
    const f32 c4 = m.at(2, 1) * m.at(3, 3) - m.at(3, 1) * m.at(2, 3);
    const f32 c3 = m.at(2, 1) * m.at(3, 2) - m.at(3, 1) * m.at(2, 2);
    const f32 c2 = m.at(2, 0) * m.at(3, 3) - m.at(3, 0) * m.at(2, 3);
    const f32 c1 = m.at(2, 0) * m.at(3, 2) - m.at(3, 0) * m.at(2, 2);
    const f32 c0 = m.at(2, 0) * m.at(3, 1) - m.at(3, 0) * m.at(2, 1);

    const f32 det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
    if (std::fabs(det) < kSingularDeterminant) {
        return cy::fail(ErrorCode::InvalidArgument, "inverse(Mat4): the matrix is singular");
    }
    const f32 d = 1.0f / det;

    Mat4 out;
    out.at(0, 0) = (m.at(1, 1) * c5 - m.at(1, 2) * c4 + m.at(1, 3) * c3) * d;
    out.at(0, 1) = (-m.at(0, 1) * c5 + m.at(0, 2) * c4 - m.at(0, 3) * c3) * d;
    out.at(0, 2) = (m.at(3, 1) * s5 - m.at(3, 2) * s4 + m.at(3, 3) * s3) * d;
    out.at(0, 3) = (-m.at(2, 1) * s5 + m.at(2, 2) * s4 - m.at(2, 3) * s3) * d;

    out.at(1, 0) = (-m.at(1, 0) * c5 + m.at(1, 2) * c2 - m.at(1, 3) * c1) * d;
    out.at(1, 1) = (m.at(0, 0) * c5 - m.at(0, 2) * c2 + m.at(0, 3) * c1) * d;
    out.at(1, 2) = (-m.at(3, 0) * s5 + m.at(3, 2) * s2 - m.at(3, 3) * s1) * d;
    out.at(1, 3) = (m.at(2, 0) * s5 - m.at(2, 2) * s2 + m.at(2, 3) * s1) * d;

    out.at(2, 0) = (m.at(1, 0) * c4 - m.at(1, 1) * c2 + m.at(1, 3) * c0) * d;
    out.at(2, 1) = (-m.at(0, 0) * c4 + m.at(0, 1) * c2 - m.at(0, 3) * c0) * d;
    out.at(2, 2) = (m.at(3, 0) * s4 - m.at(3, 1) * s2 + m.at(3, 3) * s0) * d;
    out.at(2, 3) = (-m.at(2, 0) * s4 + m.at(2, 1) * s2 - m.at(2, 3) * s0) * d;

    out.at(3, 0) = (-m.at(1, 0) * c3 + m.at(1, 1) * c1 - m.at(1, 2) * c0) * d;
    out.at(3, 1) = (m.at(0, 0) * c3 - m.at(0, 1) * c1 + m.at(0, 2) * c0) * d;
    out.at(3, 2) = (-m.at(3, 0) * s3 + m.at(3, 1) * s1 - m.at(3, 2) * s0) * d;
    out.at(3, 3) = (m.at(2, 0) * s3 - m.at(2, 1) * s1 + m.at(2, 2) * s0) * d;
    return out;
}

Expected<Mat4, Error> inverse_affine(const Mat4& m) noexcept {
    CY_ASSERT_MSG(
        m.at(3, 0) == 0.0f && m.at(3, 1) == 0.0f && m.at(3, 2) == 0.0f && m.at(3, 3) == 1.0f,
        "inverse_affine() on a matrix whose bottom row is not (0, 0, 0, 1)");
    const Expected<Mat3, Error> linear = inverse(m.upper3x3());
    if (!linear) {
        return cy::fail(ErrorCode::InvalidArgument,
                        "inverse_affine(): the matrix's linear part is singular");
    }
    const Vec3 t = *linear * m.translation();
    Mat4 out = Mat4::from_mat3(*linear);
    out.columns[3] = Vec4{-t.x, -t.y, -t.z, 1.0f};
    return out;
}

bool nearly_equal(const Mat3& a, const Mat3& b, f32 tolerance) noexcept {
    for (usize c = 0; c < 3; ++c) {
        if (!nearly_equal(a.columns[c], b.columns[c], tolerance)) {
            return false;
        }
    }
    return true;
}

bool nearly_equal(const Mat4& a, const Mat4& b, f32 tolerance) noexcept {
    for (usize c = 0; c < 4; ++c) {
        if (!nearly_equal(a.columns[c], b.columns[c], tolerance)) {
            return false;
        }
    }
    return true;
}

}  // namespace cy
