// The physics vocabulary's non-inline half: the enumerator names, the combine rule, and the two
// validations. Task 4.2.1.

#include <cy/servers/physics/types.h>

#include <cy/core/math/scalar.h>

namespace cy::physics {

const char* motion_type_name(MotionType value) noexcept {
    switch (value) {
        case MotionType::Static:
            return "static";
        case MotionType::Kinematic:
            return "kinematic";
        case MotionType::Dynamic:
            return "dynamic";
    }
    return "unknown";
}

const char* combine_mode_name(CombineMode value) noexcept {
    switch (value) {
        case CombineMode::Average:
            return "average";
        case CombineMode::Minimum:
            return "minimum";
        case CombineMode::Multiply:
            return "multiply";
        case CombineMode::Maximum:
            return "maximum";
    }
    return "unknown";
}

const char* determinism_policy_name(DeterminismPolicy value) noexcept {
    switch (value) {
        case DeterminismPolicy::SamePlatformDeterministic:
            return "same-platform-deterministic";
        case DeterminismPolicy::ExternalAuthority:
            return "external-authority";
        case DeterminismPolicy::NonAuthoritative:
            return "non-authoritative";
    }
    return "unknown";
}

f32 combine(f32 a, CombineMode a_mode, f32 b, CombineMode b_mode) noexcept {
    // The more restrictive mode wins, where "more restrictive" is the enumeration order: a Minimum
    // surface stays minimum however it is struck, and a Maximum one only wins against Average and
    // Multiply. Stated as a max over the enumerators rather than as a 4x4 table because the table
    // is the same rule written sixteen times, and because two backends must not disagree about the
    // asymmetric cases.
    const u8 mode = static_cast<u8>(a_mode) > static_cast<u8>(b_mode) ? static_cast<u8>(a_mode)
                                                                      : static_cast<u8>(b_mode);
    switch (static_cast<CombineMode>(mode)) {
        case CombineMode::Average:
            return (a + b) * 0.5f;
        case CombineMode::Minimum:
            return a < b ? a : b;
        case CombineMode::Multiply:
            return a * b;
        case CombineMode::Maximum:
            return a > b ? a : b;
    }
    return (a + b) * 0.5f;
}

Status validate(const Tuning& tuning) noexcept {
    if (tuning.velocity_iterations == 0) {
        return fail(ErrorCode::InvalidArgument, "physics tuning: velocity_iterations is zero");
    }
    if (tuning.position_iterations == 0) {
        return fail(ErrorCode::InvalidArgument, "physics tuning: position_iterations is zero");
    }
    if (tuning.penetration_slop < 0.0f) {
        return fail(ErrorCode::InvalidArgument, "physics tuning: penetration_slop is negative");
    }
    if (tuning.baumgarte < 0.0f || tuning.baumgarte > 1.0f) {
        return fail(ErrorCode::InvalidArgument, "physics tuning: baumgarte is outside [0, 1]");
    }
    if (tuning.speculative_contact_distance < 0.0f) {
        return fail(ErrorCode::InvalidArgument,
                    "physics tuning: speculative_contact_distance is negative");
    }
    // A slop deeper than the speculative distance means a contact is created only after the bodies
    // are already allowed to overlap, so the solver never sees the approach it was meant to catch.
    // The two numbers are independently plausible and wrong together, which is why this is checked
    // rather than left to each one's own range.
    if (tuning.penetration_slop > tuning.speculative_contact_distance &&
        tuning.speculative_contact_distance > 0.0f) {
        return fail(ErrorCode::InvalidArgument,
                    "physics tuning: penetration_slop exceeds speculative_contact_distance, so a "
                    "contact is only created after the overlap it was meant to prevent");
    }
    if (tuning.sleep_linear_velocity < 0.0f || tuning.sleep_angular_velocity < 0.0f ||
        tuning.time_before_sleep_seconds < 0.0f) {
        return fail(ErrorCode::InvalidArgument, "physics tuning: a sleep threshold is negative");
    }
    return ok();
}

Status validate(const WorldDescription& description) noexcept {
    if (description.body_capacity == 0) {
        return fail(ErrorCode::InvalidArgument, "physics world: body_capacity is zero");
    }
    if (description.body_pair_capacity == 0) {
        return fail(ErrorCode::InvalidArgument, "physics world: body_pair_capacity is zero");
    }
    if (description.contact_constraint_capacity == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "physics world: contact_constraint_capacity is zero");
    }
    if (!math::is_finite(description.gravity.x) || !math::is_finite(description.gravity.y) ||
        !math::is_finite(description.gravity.z)) {
        return fail(ErrorCode::InvalidArgument, "physics world: gravity is not finite");
    }
    return validate(description.tuning);
}

}  // namespace cy::physics
