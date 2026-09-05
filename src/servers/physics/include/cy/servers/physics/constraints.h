#pragma once
// Constraints: the joint kinds, their limits, motors and break thresholds. Task 4.2.1.
//
// `physics` — "Constraints": "fixed, point (ball-and-socket), hinge (with limits and a motor),
// slider, distance, cone, swing-twist, six-degrees-of-freedom (per-axis limits, motors, and
// springs), and rack-and-pinion / gear where the backend supports them", each with "a breakable
// option with a force or torque threshold".
//
// ONE DESCRIPTION, AGAIN, and for the reason shapes.h gives: a joint crosses a scene file and the
// ABI. What differs here is that the six-degrees-of-freedom kind subsumes most of the others, so
// the specialised kinds are kept rather than collapsed — a hinge is what an author means, and a
// backend maps it onto its own hinge, which is both faster and better conditioned than a 6DOF
// with four axes locked.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/transform.h>
#include <cy/servers/physics/handles.h>

namespace cy::physics {

enum class ConstraintType : u8 {
    Fixed = 0,
    Point,
    Hinge,
    Slider,
    Distance,
    Cone,
    SwingTwist,
    SixDof,
    /// `physics`: "where the backend supports them". `Capabilities::constraints` is a coarse flag;
    /// creating one a backend lacks returns `Unsupported` naming the kind.
    RackAndPinion,
    Gear,
};

const char* constraint_type_name(ConstraintType value) noexcept;

/// A driven axis. A motor with `max_force == 0` is off, which is the state a description that never
/// mentions a motor is in.
struct MotorSettings {
    /// Target speed, in metres or radians per second.
    f32 target_velocity = 0.0f;
    /// Target position, in metres or radians. Used only when `position_driven`.
    f32 target_position = 0.0f;
    bool position_driven = false;
    /// The cap `physics`' "Motorised hinge" scenario requires: the solver drives towards the target
    /// "within that limit".
    f32 max_force = 0.0f;
    /// A position-driven motor's spring. Zero frequency is a rigid drive.
    f32 spring_frequency = 0.0f;
    f32 spring_damping = 1.0f;
};

/// A range on one axis. `min > max` is free, which is how an unlimited axis is spelled without a
/// second boolean.
struct AxisLimit {
    f32 min = 1.0f;
    f32 max = -1.0f;

    [[nodiscard]] constexpr bool limited() const noexcept { return min <= max; }
};

struct ConstraintDescription {
    ConstraintType type = ConstraintType::Fixed;
    BodyHandle body_a;
    /// Null attaches `body_a` to the world.
    BodyHandle body_b;

    /// The joint frame in each body's local space. Its origin is the anchor, its axes are the
    /// joint's axes: for a hinge the local X is the hinge axis, for a slider the local X is the
    /// travel direction, for swing-twist the local X is the twist axis. One convention across every
    /// kind, so a reader learns it once.
    Transform frame_a;
    Transform frame_b;

    /// Hinge and slider: the travel range, in radians or metres.
    AxisLimit limit;
    MotorSettings motor;

    /// Distance: the range the anchors are held between. `min == max` is a rigid rod.
    f32 min_distance = 0.0f;
    f32 max_distance = 0.0f;

    /// Cone and swing-twist: the half-angles, in radians.
    f32 swing_limit_y = 0.0f;
    f32 swing_limit_z = 0.0f;
    AxisLimit twist_limit;

    /// SixDof: per-axis limits and motors, linear XYZ then angular XYZ.
    AxisLimit dof_limits[6] = {};
    MotorSettings dof_motors[6] = {};

    /// RackAndPinion and Gear: the ratio between the two bodies' motion.
    f32 ratio = 1.0f;

    /// `physics` — "Breakable joint": above this the constraint is disabled and a
    /// `ConstraintBroken` event is emitted. Zero, the default, never breaks.
    f32 break_force = 0.0f;
    f32 break_torque = 0.0f;

    /// Whether the two bodies collide with each other while joined. Off by default, because a
    /// joint's own bodies usually overlap at the anchor.
    bool collide_connected = false;

    UserData user_data = 0;
};

[[nodiscard]] Status validate(const ConstraintDescription& description) noexcept;

/// What `physics`' "Breakable joint" scenario emits. Read alongside the contact events, from the
/// same step.
struct ConstraintBroken {
    ConstraintHandle constraint;
    UserData user_data = 0;
    /// The force and torque the solver measured on the step it broke.
    f32 force = 0.0f;
    f32 torque = 0.0f;
};

}  // namespace cy::physics
