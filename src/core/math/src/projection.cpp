// Projections and view matrices. Task 3.1.2. See include/cy/core/math/projection.h.
//
// Every matrix here is written column by column, and each is annotated with the mapping it produces
// at the near and far planes, because those two numbers are the whole of the reversed-Z convention
// and tests/test_conventions.cpp asserts them exactly.

#include <cy/core/math/projection.h>

#include <cmath>

namespace cy {

Mat4 perspective_reversed_z(f32 fov_y_radians, f32 aspect, f32 near_plane, f32 far_plane) noexcept {
    CY_ASSERT_MSG(fov_y_radians > 0.0f && fov_y_radians < math::kPi,
                  "perspective_reversed_z(): the vertical field of view must be in (0, pi)");
    CY_ASSERT_MSG(aspect > 0.0f, "perspective_reversed_z(): aspect must be positive");
    CY_ASSERT_MSG(near_plane > 0.0f && far_plane > near_plane,
                  "perspective_reversed_z(): needs 0 < near < far");

    const f32 focal = 1.0f / std::tan(fov_y_radians * 0.5f);
    const f32 span = far_plane - near_plane;

    // The conventional [0,1] projection has m22 = far/(near-far) and m32 = near*far/(near-far).
    // Reversing the depth is exactly swapping near and far in those two entries, which is why the
    // whole convention costs no arithmetic at all — only the discipline to write it down once.
    //
    //   z_view = -near : clip.z = near,  clip.w = near  ->  depth = 1
    //   z_view = -far  : clip.z = 0,     clip.w = far   ->  depth = 0
    Mat4 m = Mat4::zero();
    m.at(0, 0) = focal / aspect;
    m.at(1, 1) = focal;
    m.at(2, 2) = near_plane / span;
    m.at(2, 3) = far_plane * near_plane / span;
    // Right-handed, looking down -Z: clip.w is -z_view, so a point in front of the camera has a
    // positive w and survives the divide.
    m.at(3, 2) = -1.0f;
    return m;
}

Mat4 perspective_reversed_z_infinite(f32 fov_y_radians, f32 aspect, f32 near_plane) noexcept {
    CY_ASSERT_MSG(
        fov_y_radians > 0.0f && fov_y_radians < math::kPi,
        "perspective_reversed_z_infinite(): the vertical field of view must be in (0, pi)");
    CY_ASSERT_MSG(aspect > 0.0f, "perspective_reversed_z_infinite(): aspect must be positive");
    CY_ASSERT_MSG(near_plane > 0.0f, "perspective_reversed_z_infinite(): near must be positive");

    const f32 focal = 1.0f / std::tan(fov_y_radians * 0.5f);

    // The limit of the finite form as far -> infinity: near/(far-near) -> 0 and
    // far*near/(far-near) -> near. Depth is then simply near / -z_view, which is 1 at the near
    // plane and approaches 0 without reaching it.
    Mat4 m = Mat4::zero();
    m.at(0, 0) = focal / aspect;
    m.at(1, 1) = focal;
    m.at(2, 2) = 0.0f;
    m.at(2, 3) = near_plane;
    m.at(3, 2) = -1.0f;
    return m;
}

Mat4 orthographic_reversed_z(f32 left, f32 right, f32 bottom, f32 top, f32 near_plane,
                             f32 far_plane) noexcept {
    CY_ASSERT_MSG(right != left && top != bottom && far_plane != near_plane,
                  "orthographic_reversed_z(): a degenerate volume has no projection");

    const f32 span = far_plane - near_plane;

    //   z_view = -near : depth = (-(-near) ... ) = 1
    //   z_view = -far  : depth = 0
    // Solving depth = a*z_view + b for those two gives a = 1/(far-near), b = far/(far-near).
    Mat4 m = Mat4::identity();
    m.at(0, 0) = 2.0f / (right - left);
    m.at(1, 1) = 2.0f / (top - bottom);
    m.at(2, 2) = 1.0f / span;
    m.at(0, 3) = -(right + left) / (right - left);
    m.at(1, 3) = -(top + bottom) / (top - bottom);
    m.at(2, 3) = far_plane / span;
    return m;
}

Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up) noexcept {
    // The camera's local basis. z_axis points *backwards* along the view direction, because the
    // camera looks down its own -Z.
    const Vec3 z_axis = normalize(eye - target);
    const Vec3 x_axis = normalize(cross(up, z_axis));
    const Vec3 y_axis = cross(z_axis, x_axis);

    // The view matrix is the inverse of the camera's placement. For an orthonormal basis the
    // inverse rotation is the transpose, so the basis vectors become the *rows*, and the
    // translation is minus the eye expressed in the camera's own axes.
    Mat4 m = Mat4::identity();
    m.at(0, 0) = x_axis.x;
    m.at(0, 1) = x_axis.y;
    m.at(0, 2) = x_axis.z;
    m.at(1, 0) = y_axis.x;
    m.at(1, 1) = y_axis.y;
    m.at(1, 2) = y_axis.z;
    m.at(2, 0) = z_axis.x;
    m.at(2, 1) = z_axis.y;
    m.at(2, 2) = z_axis.z;
    m.at(0, 3) = -dot(x_axis, eye);
    m.at(1, 3) = -dot(y_axis, eye);
    m.at(2, 3) = -dot(z_axis, eye);
    return m;
}

Mat4 view_from_transform(const Quat& rotation, Vec3 position) noexcept {
    const Quat inverse_rotation = conjugate(normalize(rotation));
    Mat4 m = Mat4::from_quat(inverse_rotation);
    const Vec3 t = inverse_rotation * -position;
    m.columns[3] = Vec4{t.x, t.y, t.z, 1.0f};
    return m;
}

void perspective_planes(const Mat4& projection, f32& near_plane, f32& far_plane) noexcept {
    // Inverting the two entries the constructors wrote: m22 = near/(far-near) and
    // m23 = far*near/(far-near). Their ratio is 1/far, and m23 - m22*near = ... solving gives:
    const f32 m22 = projection.at(2, 2);
    const f32 m23 = projection.at(2, 3);
    if (m22 == 0.0f) {
        // The infinite form: m23 is the near plane outright.
        near_plane = m23;
        far_plane = math::kInfinity;
        return;
    }
    far_plane = m23 / m22;
    near_plane = m23 / (m22 + 1.0f);
}

}  // namespace cy
