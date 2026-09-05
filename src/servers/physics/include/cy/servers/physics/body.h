#pragma once
// Bodies and colliders: what one is created from, and what the solver reports back. Task 4.2.1.
//
// `physics` — "Physics components" names `RigidBody`, `StaticBody`, `KinematicBody`, `Collider`,
// `Trigger` and `PhysicsMaterial`. On the *interface* those are one `BodyDescription` with a
// `MotionType` and a list of `ColliderDescription`s, because a solver has one body concept and
// three components mapping onto it is an ECS convenience (components.h), not a solver one.
//
// THE 2D STRATEGY LIVES IN `locked_axes`. `physics` — "2D physics": 2D is "the 3D backend
// constrained to a plane: bodies are created with locked Z translation and locked X/Y rotation".
// That is one field, not a second body kind, which is what makes the constraint enforceable by the
// solver rather than by a fix-up after every step. physics2d.h is the 2D API that sets it.

#include <cy/core/base/types.h>
#include <cy/core/math/transform.h>
#include <cy/core/values/name.h>
#include <cy/servers/physics/handles.h>
#include <cy/servers/physics/types.h>

namespace cy::physics {

// --- Degrees of freedom ------------------------------------------------------------------------

/// A bit per locked axis. `physics`' "Constraint is enforced" scenario — a 2D body subjected to a
/// force with a Z component shows "no out-of-plane motion or drift" — is exactly this being applied
/// by the integrator, every step, rather than by a correction that runs afterwards and leaves the
/// velocity behind.
inline constexpr u8 kLockLinearX = 1U << 0U;
inline constexpr u8 kLockLinearY = 1U << 1U;
inline constexpr u8 kLockLinearZ = 1U << 2U;
inline constexpr u8 kLockAngularX = 1U << 3U;
inline constexpr u8 kLockAngularY = 1U << 4U;
inline constexpr u8 kLockAngularZ = 1U << 5U;

/// The XY-plane lock: no Z translation, no X or Y rotation. What physics2d.h creates every body
/// with, and the whole of `physics`' constrained-3D strategy.
inline constexpr u8 kLockPlaneXY = kLockLinearZ | kLockAngularX | kLockAngularY;

/// Apply the locks to a linear vector, in world space.
[[nodiscard]] constexpr Vec3 apply_linear_locks(Vec3 v, u8 locks) noexcept {
    return Vec3{(locks & kLockLinearX) != 0U ? 0.0f : v.x,
                (locks & kLockLinearY) != 0U ? 0.0f : v.y,
                (locks & kLockLinearZ) != 0U ? 0.0f : v.z};
}

[[nodiscard]] constexpr Vec3 apply_angular_locks(Vec3 v, u8 locks) noexcept {
    return Vec3{(locks & kLockAngularX) != 0U ? 0.0f : v.x,
                (locks & kLockAngularY) != 0U ? 0.0f : v.y,
                (locks & kLockAngularZ) != 0U ? 0.0f : v.z};
}

// --- Colliders ---------------------------------------------------------------------------------

/// One shape attached to a body.
///
/// `physics` — "Multiple colliders per body": several "SHALL form a compound shape with a combined
/// mass distribution computed from their volumes and the body's density or explicit mass". The
/// interface takes the list; the backend forms the compound.
struct ColliderDescription {
    ShapeHandle shape;
    /// Where the shape sits relative to the body's origin.
    Transform local;
    /// Null uses the world's default material.
    MaterialHandle material;
    CollisionFilter filter;
    /// A sensor: reports overlap and never resolves. `physics` — the `Trigger` component.
    bool is_trigger = false;
    /// `physics` — "Contact filtering": "WHEN a body enables contact reporting only above an
    /// impulse threshold THEN events below the threshold SHALL not be generated, avoiding event
    /// floods on resting contacts". In newton-seconds; zero reports everything.
    f32 contact_impulse_threshold = 0.0f;
    /// False suppresses `CollisionStay` for this collider, keeping enter and exit. The cheap half
    /// of the same problem: a resting stack costs one event per pair per tick forever.
    bool report_stay = true;
};

// --- Bodies ------------------------------------------------------------------------------------

struct BodyDescription {
    Name name;
    MotionType motion = MotionType::Dynamic;
    Transform transform;
    Vec3 linear_velocity{0.0f, 0.0f, 0.0f};
    Vec3 angular_velocity{0.0f, 0.0f, 0.0f};

    /// Zero derives the mass from the colliders' volumes and densities, which is what a body
    /// usually wants. A non-zero value overrides it.
    f32 mass = 0.0f;
    /// Set with `override_center_of_mass`; otherwise the centre is derived from the colliders.
    Vec3 center_of_mass{0.0f, 0.0f, 0.0f};
    bool override_center_of_mass = false;

    f32 linear_damping = 0.05f;
    f32 angular_damping = 0.05f;
    f32 gravity_scale = 1.0f;

    bool allow_sleeping = true;
    bool start_asleep = false;
    /// Continuous collision detection. Costs a sweep per step and is what a bullet needs.
    bool continuous = false;

    /// See kLock* above.
    u8 locked_axes = 0;

    /// Not owned; read during the call.
    const ColliderDescription* colliders = nullptr;
    u32 collider_count = 0;

    /// 64 bits the caller owns the meaning of. The server copies it and never dereferences it —
    /// see handles.h.
    UserData user_data = 0;
};

[[nodiscard]] Status validate(const BodyDescription& description) noexcept;

/// What the solver reports back after a step.
struct BodyState {
    Transform transform;
    Vec3 linear_velocity{0.0f, 0.0f, 0.0f};
    Vec3 angular_velocity{0.0f, 0.0f, 0.0f};
    MotionType motion = MotionType::Dynamic;
    bool asleep = false;
    /// Set by `set_body_transform(..., TeleportMode::Teleport)` and cleared by the next step. Read
    /// by the interpolation in stepper.h, which is the only consumer that needs it.
    bool teleported = false;
};

/// The mass properties a body ended up with, which is what a test asserts when it wants to know
/// that a compound's mass distribution was combined rather than taken from the first collider.
struct MassProperties {
    f32 mass = 0.0f;
    Vec3 center_of_mass{0.0f, 0.0f, 0.0f};
    /// The diagonal of the inertia tensor about the centre of mass, in the body's local frame.
    Vec3 inertia{0.0f, 0.0f, 0.0f};
};

}  // namespace cy::physics
