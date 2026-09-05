// Constraint names and validation. Task 4.2.1.

#include <cy/servers/physics/constraints.h>

#include <cy/core/math/scalar.h>

namespace cy::physics {

const char* constraint_type_name(ConstraintType value) noexcept {
    switch (value) {
        case ConstraintType::Fixed:
            return "fixed";
        case ConstraintType::Point:
            return "point";
        case ConstraintType::Hinge:
            return "hinge";
        case ConstraintType::Slider:
            return "slider";
        case ConstraintType::Distance:
            return "distance";
        case ConstraintType::Cone:
            return "cone";
        case ConstraintType::SwingTwist:
            return "swing-twist";
        case ConstraintType::SixDof:
            return "six-dof";
        case ConstraintType::RackAndPinion:
            return "rack-and-pinion";
        case ConstraintType::Gear:
            return "gear";
    }
    return "unknown";
}

Status validate(const ConstraintDescription& d) noexcept {
    if (d.body_a.is_null()) {
        return fail(ErrorCode::InvalidArgument, "constraint: body_a is null");
    }
    if (d.body_a == d.body_b) {
        return fail(ErrorCode::InvalidArgument, "constraint: a body cannot be joined to itself");
    }
    if (d.break_force < 0.0f || d.break_torque < 0.0f) {
        return fail(ErrorCode::InvalidArgument, "constraint: a break threshold is negative");
    }
    if (d.type == ConstraintType::Distance && d.min_distance > d.max_distance) {
        return fail(ErrorCode::InvalidArgument,
                    "distance constraint: min_distance exceeds max_distance");
    }
    if ((d.type == ConstraintType::RackAndPinion || d.type == ConstraintType::Gear) &&
        (d.ratio == 0.0f || !math::is_finite(d.ratio))) {
        return fail(ErrorCode::InvalidArgument, "gear constraint: ratio must be non-zero");
    }
    if (d.motor.max_force < 0.0f) {
        return fail(ErrorCode::InvalidArgument, "constraint motor: max_force is negative");
    }
    return ok();
}

}  // namespace cy::physics
