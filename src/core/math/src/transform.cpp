// Transform and Transform2D's out-of-line operations. Task 3.1.1.
// See include/cy/core/math/transform.h.

#include <cy/core/math/transform.h>

#include <cmath>

namespace cy {

Transform operator*(const Transform& a, const Transform& b) noexcept {
    // b applied first: a point goes through b's scale, rotation and translation, then a's. Writing
    // the composition out rather than composing matrices is what keeps the result exact for the
    // rotation and translation parts.
    Transform out;
    out.rotation = a.rotation * b.rotation;
    out.scale = cwise_mul(a.scale, b.scale);
    out.translation = a.transform_point(b.translation);
    return out;
}

Transform inverse(const Transform& t) noexcept {
    Transform out;
    out.rotation = conjugate(normalize(t.rotation));
    // A zero on any scale axis is not invertible. Leaving it zero rather than producing an infinity
    // keeps the result finite and wrong-in-an-obvious-way instead of NaN-propagating through
    // whatever consumes it; a caller that cares tests the scale first.
    out.scale = Vec3{t.scale.x != 0.0f ? 1.0f / t.scale.x : 0.0f,
                     t.scale.y != 0.0f ? 1.0f / t.scale.y : 0.0f,
                     t.scale.z != 0.0f ? 1.0f / t.scale.z : 0.0f};
    out.translation = out.rotation * cwise_mul(out.scale, -t.translation);
    return out;
}

Transform interpolate(const Transform& a, const Transform& b, f32 t) noexcept {
    Transform out;
    out.rotation = slerp(a.rotation, b.rotation, t);
    out.translation = lerp(a.translation, b.translation, t);
    out.scale = lerp(a.scale, b.scale, t);
    return out;
}

Expected<Transform, Error> decompose(const Mat4& m) noexcept {
    if (m.at(3, 0) != 0.0f || m.at(3, 1) != 0.0f || m.at(3, 2) != 0.0f || m.at(3, 3) != 1.0f) {
        return cy::fail(ErrorCode::InvalidArgument,
                        "decompose(): the matrix is not affine (its bottom row is not 0,0,0,1)");
    }

    Vec3 c0 = m.columns[0].xyz();
    Vec3 c1 = m.columns[1].xyz();
    Vec3 c2 = m.columns[2].xyz();

    Vec3 scale{length(c0), length(c1), length(c2)};
    if (scale.x < math::kSmallLength || scale.y < math::kSmallLength ||
        scale.z < math::kSmallLength) {
        return cy::fail(ErrorCode::InvalidArgument,
                        "decompose(): the matrix's linear part is singular");
    }

    // A negative determinant means the matrix mirrors. A mirror is not a rotation, so it has to be
    // attributed to the scale; the choice of which axis carries the sign is arbitrary, and X is the
    // conventional one. Without this the basis handed to Quat::from_basis would be left-handed and
    // the recovered quaternion meaningless.
    if (determinant(m.upper3x3()) < 0.0f) {
        scale.x = -scale.x;
    }

    c0 = c0 * (1.0f / scale.x);
    c1 = c1 * (1.0f / scale.y);
    c2 = c2 * (1.0f / scale.z);

    Transform out;
    out.translation = m.translation();
    out.scale = scale;
    out.rotation = Quat::from_basis(c0, c1, c2);
    return out;
}

// --- Transform2D --------------------------------------------------------------------------------

Vec2 Transform2D::transform_point(Vec2 p) const noexcept {
    const f32 c = std::cos(rotation_radians);
    const f32 s = std::sin(rotation_radians);
    const Vec2 scaled = cwise_mul(scale, p);
    return translation + Vec2{scaled.x * c - scaled.y * s, scaled.x * s + scaled.y * c};
}

Vec2 Transform2D::transform_vector(Vec2 v) const noexcept {
    const f32 c = std::cos(rotation_radians);
    const f32 s = std::sin(rotation_radians);
    const Vec2 scaled = cwise_mul(scale, v);
    return Vec2{scaled.x * c - scaled.y * s, scaled.x * s + scaled.y * c};
}

Mat3 Transform2D::to_matrix() const noexcept {
    const f32 c = std::cos(rotation_radians);
    const f32 s = std::sin(rotation_radians);
    // The same column-vector convention as Mat4: the translation is the last column.
    return Mat3{{Vec3{c * scale.x, s * scale.x, 0.0f}, Vec3{-s * scale.y, c * scale.y, 0.0f},
                 Vec3{translation.x, translation.y, 1.0f}}};
}

Transform2D operator*(const Transform2D& a, const Transform2D& b) noexcept {
    Transform2D out;
    out.rotation_radians = a.rotation_radians + b.rotation_radians;
    out.scale = cwise_mul(a.scale, b.scale);
    out.translation = a.transform_point(b.translation);
    return out;
}

Transform2D inverse(const Transform2D& t) noexcept {
    Transform2D out;
    out.rotation_radians = -t.rotation_radians;
    out.scale = Vec2{t.scale.x != 0.0f ? 1.0f / t.scale.x : 0.0f,
                     t.scale.y != 0.0f ? 1.0f / t.scale.y : 0.0f};
    out.translation = Vec2{};
    out.translation = out.transform_point(-t.translation);
    return out;
}

Transform2D interpolate(const Transform2D& a, const Transform2D& b, f32 t) noexcept {
    Transform2D out;
    // The rotation difference is wrapped so that interpolating from 350° to 10° takes the short way
    // round rather than sweeping backwards through the whole circle. `Quat` slerp does the
    // equivalent by choosing the nearer hemisphere; this is the scalar version of the same rule.
    const f32 delta = math::wrap_angle(b.rotation_radians - a.rotation_radians);
    out.rotation_radians = a.rotation_radians + delta * t;
    out.translation = lerp(a.translation, b.translation, t);
    out.scale = lerp(a.scale, b.scale, t);
    return out;
}

}  // namespace cy
