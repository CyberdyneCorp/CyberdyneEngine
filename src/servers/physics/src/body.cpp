// Body validation, and the one rejection `physics` calls out by name. Task 4.2.1.

#include <cy/servers/physics/body.h>

#include <cy/core/math/scalar.h>

namespace cy::physics {

Status validate(const BodyDescription& d) noexcept {
    if (d.mass < 0.0f || !math::is_finite(d.mass)) {
        return fail(ErrorCode::InvalidArgument,
                    "body: mass must be zero (derive it) or a positive finite number");
    }
    if (d.gravity_scale != 0.0f && !math::is_finite(d.gravity_scale)) {
        return fail(ErrorCode::InvalidArgument, "body: gravity_scale is not finite");
    }
    if (d.linear_damping < 0.0f || d.angular_damping < 0.0f) {
        return fail(ErrorCode::InvalidArgument, "body: damping must not be negative");
    }
    if (d.collider_count != 0 && d.colliders == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "body: collider_count is non-zero and colliders is null");
    }
    for (u32 index = 0; index < d.collider_count; ++index) {
        if (d.colliders[index].shape.is_null()) {
            return fail(ErrorCode::InvalidArgument, "body: a collider has a null shape");
        }
    }
    if (d.motion == MotionType::Dynamic && d.collider_count == 0) {
        // A dynamic body with no collider has no volume, therefore no derived mass, therefore an
        // infinite acceleration the first time gravity is applied. It is always a mistake, and it
        // is one that shows up as a NaN position rather than as anything that names a body.
        return fail(ErrorCode::InvalidArgument,
                    "body: a dynamic body needs at least one collider to derive its mass from");
    }
    return ok();
}

}  // namespace cy::physics
