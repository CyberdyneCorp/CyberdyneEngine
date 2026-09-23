#pragma once
// Engine-owned cloth description and readback. A backend may decline this optional simulation.

#include <cy/core/base/expected.h>
#include <cy/core/math/transform.h>
#include <cy/servers/physics/handles.h>
#include <cy/servers/physics/types.h>

namespace cy::physics {

struct SoftBodyVertex {
    Vec3 position;
    /// Zero pins this vertex; positive values allow simulation.
    f32 inverse_mass = 1.0f;
};

struct SoftBodyDescription {
    Transform transform;
    /// Rest-pose vertices and triangle indices are borrowed only for create_soft_body().
    const SoftBodyVertex* vertices = nullptr;
    u32 vertex_count = 0;
    const u32* indices = nullptr;
    u32 index_count = 0;
    CollisionFilter filter;
    f32 damping = 0.1f;
    f32 friction = 0.2f;
    f32 vertex_radius = 0.01f;
    u32 solver_iterations = 5;
    bool allow_sleeping = true;
    UserData user_data = 0;
};

/// Reject malformed topology before allocating a backend body.
[[nodiscard]] Status validate(const SoftBodyDescription& description) noexcept;

}  // namespace cy::physics
