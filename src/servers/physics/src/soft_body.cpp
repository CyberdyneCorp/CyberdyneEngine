// SPDX-License-Identifier: MIT
#include <cy/servers/physics/soft_body.h>

#include <cy/core/math/scalar.h>

namespace cy::physics {

Status validate(const SoftBodyDescription& description) noexcept {
    if (description.vertices == nullptr || description.vertex_count < 3 ||
        description.indices == nullptr || description.index_count == 0 ||
        description.index_count % 3 != 0) {
        return fail(ErrorCode::InvalidArgument, "physics cloth: expected vertices and triangles");
    }
    if (description.filter.layer >= kCollisionLayerCount || !math::is_finite(description.damping) ||
        description.damping < 0.0f || !math::is_finite(description.friction) ||
        description.friction < 0.0f || !math::is_finite(description.vertex_radius) ||
        description.vertex_radius < 0.0f || description.solver_iterations == 0) {
        return fail(ErrorCode::InvalidArgument, "physics cloth: invalid solver settings");
    }
    const Transform& transform = description.transform;
    if (!math::is_finite(transform.translation.x) || !math::is_finite(transform.translation.y) ||
        !math::is_finite(transform.translation.z) || !math::is_finite(transform.rotation.x) ||
        !math::is_finite(transform.rotation.y) || !math::is_finite(transform.rotation.z) ||
        !math::is_finite(transform.rotation.w) || transform.scale.x != 1.0f ||
        transform.scale.y != 1.0f || transform.scale.z != 1.0f) {
        return fail(ErrorCode::InvalidArgument, "physics cloth: invalid rigid transform");
    }
    for (u32 vertex = 0; vertex < description.vertex_count; ++vertex) {
        const SoftBodyVertex& value = description.vertices[vertex];
        if (!math::is_finite(value.position.x) || !math::is_finite(value.position.y) ||
            !math::is_finite(value.position.z) || !math::is_finite(value.inverse_mass) ||
            value.inverse_mass < 0.0f) {
            return fail(ErrorCode::InvalidArgument, "physics cloth: invalid vertex");
        }
    }
    for (u32 index = 0; index < description.index_count; index += 3) {
        const u32 a = description.indices[index];
        const u32 b = description.indices[index + 1];
        const u32 c = description.indices[index + 2];
        if (a >= description.vertex_count || b >= description.vertex_count ||
            c >= description.vertex_count || a == b || a == c || b == c) {
            return fail(ErrorCode::InvalidArgument, "physics cloth: invalid triangle");
        }
    }
    return ok();
}

}  // namespace cy::physics
