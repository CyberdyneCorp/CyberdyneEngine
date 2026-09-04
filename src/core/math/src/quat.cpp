// Quat's out-of-line operations. Task 3.1.1. See include/cy/core/math/quat.h.

#include <cy/core/math/quat.h>

#include <cmath>

namespace cy {
namespace {

/// Beyond this, `slerp`'s division by sin(theta) loses precision faster than the linear
/// interpolation it would replace costs in angular-velocity error. cos(theta) = 0.9995 is an angle
/// of about 1.8 degrees, over which the difference between the two is far below what a frame of
/// animation can show.
constexpr f32 kSlerpLinearThreshold = 0.9995f;

}  // namespace

Quat Quat::from_euler_yxz(Vec3 euler_radians) noexcept {
    const Quat yaw = from_axis_angle(kAxisY, euler_radians.y);
    const Quat pitch = from_axis_angle(kAxisX, euler_radians.x);
    const Quat roll = from_axis_angle(kAxisZ, euler_radians.z);
    // Right to left is application order: roll first, then pitch, then yaw — which is what YXZ
    // names when read as the order the matrices are written in.
    return yaw * pitch * roll;
}

Quat Quat::from_basis(Vec3 x_axis, Vec3 y_axis, Vec3 z_axis) noexcept {
    // Shepperd's method: pick the branch whose divisor is largest, so the square root never
    // divides by something near zero. The naive single-branch formula loses all precision when the
    // trace approaches -1, which happens for any rotation near 180 degrees — not an edge case.
    const f32 m00 = x_axis.x;
    const f32 m10 = x_axis.y;
    const f32 m20 = x_axis.z;
    const f32 m01 = y_axis.x;
    const f32 m11 = y_axis.y;
    const f32 m21 = y_axis.z;
    const f32 m02 = z_axis.x;
    const f32 m12 = z_axis.y;
    const f32 m22 = z_axis.z;

    const f32 trace = m00 + m11 + m22;
    if (trace > 0.0f) {
        const f32 s = std::sqrt(trace + 1.0f) * 2.0f;
        return Quat{(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    }
    if (m00 > m11 && m00 > m22) {
        const f32 s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        return Quat{0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    }
    if (m11 > m22) {
        const f32 s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        return Quat{(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    }
    const f32 s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
    return Quat{(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
}

Quat Quat::look_rotation(Vec3 forward, Vec3 up) noexcept {
    // The local Z axis points *backwards*, because forward is −Z. Everything about the engine's
    // handedness that a reader might get wrong is in this one line.
    const Vec3 z_axis = normalize(-forward);
    const Vec3 x_axis = normalize(cross(up, z_axis));
    const Vec3 y_axis = cross(z_axis, x_axis);
    return from_basis(x_axis, y_axis, z_axis);
}

Quat Quat::from_to(Vec3 from, Vec3 to) noexcept {
    const f32 d = dot(from, to);
    if (d >= 1.0f - math::kEpsilon) {
        return Quat::identity();
    }
    if (d <= -1.0f + math::kEpsilon) {
        // Antiparallel: every axis perpendicular to `from` is an equally correct half-turn, so pick
        // one deterministically rather than letting a near-zero cross product choose at random.
        return from_axis_angle(any_perpendicular(from), math::kPi);
    }
    const Vec3 axis = cross(from, to);
    return normalize(Quat{axis.x, axis.y, axis.z, 1.0f + d});
}

Vec3 Quat::to_euler_yxz() const noexcept {
    // Read off the rotation matrix entries the YXZ order determines. With R = Ry·Rx·Rz:
    //   m12 = -sin(pitch), m10 = cos(pitch)·sin(roll), m11 = cos(pitch)·cos(roll),
    //   m02 = sin(yaw)·cos(pitch), m22 = cos(yaw)·cos(pitch).
    const f32 m10 = 2.0f * ((x * y) + (w * z));
    const f32 m11 = 1.0f - (2.0f * ((x * x) + (z * z)));
    const f32 m12 = 2.0f * ((y * z) - (w * x));
    const f32 m02 = 2.0f * ((x * z) + (w * y));
    const f32 m22 = 1.0f - (2.0f * ((x * x) + (y * y)));
    const f32 m00 = 1.0f - (2.0f * ((y * y) + (z * z)));
    const f32 m01 = 2.0f * ((x * y) - (w * z));

    const f32 sin_pitch = math::clamp(-m12, -1.0f, 1.0f);
    const f32 pitch = std::asin(sin_pitch);

    // Gimbal lock: pitch at ±90° collapses yaw and roll into one degree of freedom, so only their
    // difference is recoverable. Attribute all of it to yaw and set roll to zero — an arbitrary but
    // stable choice, and the one an editor wants, because the alternative makes both fields jitter.
    if (std::fabs(sin_pitch) > 1.0f - 1e-6f) {
        return Vec3{pitch, std::atan2(m01, m00), 0.0f};
    }
    return Vec3{pitch, std::atan2(m02, m22), std::atan2(m10, m11)};
}

void Quat::to_axis_angle(Vec3& axis, f32& angle_radians) const noexcept {
    const Quat q = normalize(*this);
    const f32 clamped_w = math::clamp(q.w, -1.0f, 1.0f);
    angle_radians = 2.0f * std::acos(clamped_w);
    const f32 sin_half = std::sqrt(math::max(0.0f, 1.0f - (clamped_w * clamped_w)));
    if (sin_half < math::kSmallLength) {
        // The identity, or as close as makes no difference. There is no axis, so return a valid
        // one rather than a zero vector a caller would go on to normalise.
        axis = kAxisX;
        angle_radians = 0.0f;
        return;
    }
    axis = Vec3{q.x, q.y, q.z} * (1.0f / sin_half);
}

Quat slerp(const Quat& a, const Quat& b, f32 t) noexcept {
    f32 cos_theta = dot(a, b);
    // q and -q are the same rotation; without this the interpolation takes the long way round
    // whenever the two happen to be stored on opposite hemispheres.
    Quat target = b;
    if (cos_theta < 0.0f) {
        target = -b;
        cos_theta = -cos_theta;
    }
    if (cos_theta > kSlerpLinearThreshold) {
        return nlerp(a, target, t);
    }
    const f32 theta = std::acos(math::clamp(cos_theta, -1.0f, 1.0f));
    const f32 sin_theta = std::sin(theta);
    const f32 wa = std::sin((1.0f - t) * theta) / sin_theta;
    const f32 wb = std::sin(t * theta) / sin_theta;
    return a * wa + target * wb;
}

f32 angle_between(const Quat& a, const Quat& b) noexcept {
    // |dot| rather than dot, again for the double cover: the angle between a rotation and its
    // negation is zero, not pi.
    const f32 d = math::clamp(std::fabs(dot(a, b)), -1.0f, 1.0f);
    return 2.0f * std::acos(d);
}

}  // namespace cy
