#pragma once
// The physics components, as plain data. Task 4.2.3.
//
// `physics` — "Physics components": `RigidBody`, `StaticBody`, `KinematicBody`, `Collider`,
// `CharacterController`, `Constraint`, `Trigger` and `PhysicsMaterial`, "expressed as ECS
// components".
//
// ================================================================================================
// WHY THESE STRUCTS DO NOT MENTION THE ECS, AND WHERE THE REGISTRATION LIVES
// ================================================================================================
//
// A component is two things: a layout, and a registration in a world. The LAYOUT is what a scene
// file stores, what the ABI marshals and what a Swift `@Export` maps onto, and it belongs beside
// the server whose vocabulary it is written in — a `Collider` holds a `ShapeDescription`, a
// `CollisionFilter` and a `MaterialHandle`, and all three are physics' types.
//
// The REGISTRATION belongs with whoever owns the world. `cy::scene::LocalTransform` — the component
// `physics`' fixed-step requirement says transforms are published to — is layer 4, and this
// directory is layer 2, so the bridge that reads `RigidBody`, creates a body and writes
// `LocalTransform` back cannot live here. It is `src/physics/` at layer 4, mirroring
// `src/rendering/scene/`, and it does not exist yet: see README.md, "What is not here yet".
//
// What DOES exist here is `PhysicsStepper` (stepper.h), which owns the half of that bridge the
// requirement is actually about — one step per simulation tick, and the interpolation pair — and
// publishes through a sink the layer-4 bridge implements. So the requirement is met at layer 2 and
// the ECS plumbing is a thin layer above it, rather than the requirement waiting on the plumbing.
//
// EVERY STRUCT HERE IS TRIVIALLY COPYABLE and default-constructs to something meaningful, because
// ECS chunk storage is memset and `serialization-and-prefabs` needs a default to diff overrides
// against. A zeroed `RigidBody` is a one-kilogram body with a null runtime handle — not a body with
// zero mass, which would be a division by zero the first time it was pushed.

#include <cy/core/base/types.h>
#include <cy/core/math/transform.h>
#include <cy/servers/physics/body.h>
#include <cy/servers/physics/character.h>
#include <cy/servers/physics/constraints.h>
#include <cy/servers/physics/handles.h>
#include <cy/servers/physics/shapes.h>
#include <cy/servers/physics/types.h>

#include <type_traits>

namespace cy::physics {

/// The runtime handle a component holds once the body behind it exists.
///
/// Null until the bridge creates it, and null again after a reload — which is why it is a separate
/// member rather than the component's identity. `serialization-and-prefabs` never writes it: a
/// handle is runtime-only and a scene file that stored one would restore a dangling reference.
struct BodyRef {
    BodyHandle body;
};

/// A dynamic body. `physics`' `RigidBody`.
struct RigidBody {
    /// Zero derives the mass from the colliders' volumes and densities.
    f32 mass = 0.0f;
    Vec3 center_of_mass{0.0f, 0.0f, 0.0f};
    bool override_center_of_mass = false;
    f32 linear_damping = 0.05f;
    f32 angular_damping = 0.05f;
    f32 gravity_scale = 1.0f;
    bool allow_sleeping = true;
    bool start_asleep = false;
    bool continuous = false;
    /// See body.h's kLock* constants. `kLockPlaneXY` is the 2D body.
    u8 locked_axes = 0;
    BodyHandle body;
};

/// Non-moving collision geometry. `physics`' `StaticBody`.
struct StaticBody {
    BodyHandle body;
};

/// Moved by code or animation; pushes dynamic bodies and is not pushed. `physics`' `KinematicBody`.
struct KinematicBody {
    BodyHandle body;
};

/// One shape on a body. `physics`' `Collider`; several may attach to one body, and they form a
/// compound.
struct Collider {
    ShapeDescription shape;
    Transform local;
    MaterialHandle material;
    CollisionFilter filter;
    f32 contact_impulse_threshold = 0.0f;
    bool report_stay = true;
    /// The shape the server cached for `shape`, filled by the bridge.
    ShapeHandle handle;
};

/// A non-solid volume reporting overlap. `physics`' `Trigger`.
///
/// A separate component rather than a flag on `Collider`, because `physics` lists it as one and
/// because "is this entity a trigger" is then a query the ECS answers without reading a field.
struct Trigger {
    ShapeDescription shape;
    Transform local;
    CollisionFilter filter;
    ShapeHandle handle;
};

/// Friction, restitution and their combine modes. `physics`' `PhysicsMaterial`.
struct PhysicsMaterial {
    f32 friction = 0.5f;
    f32 restitution = 0.0f;
    CombineMode friction_combine = CombineMode::Average;
    CombineMode restitution_combine = CombineMode::Average;
    f32 density = 1000.0f;
    MaterialHandle handle;
};

/// A joint between two bodies. `physics`' `Constraint`.
///
/// It holds the description by value rather than a pointer to one, so a prefab carries its joints
/// and an override can change a limit without an asset reference.
struct Joint {
    ConstraintDescription description;
    ConstraintHandle handle;
};

/// The capsule controller's authored settings. `physics`' `CharacterController`.
///
/// The controller object itself (character.h) is not a component: it holds a pointer to the server
/// and a shape handle, which makes it neither trivially copyable nor safe in chunk storage. The
/// component is the description plus the handle of the controller the bridge created for it.
struct CharacterBody {
    CharacterDescription description;
    BodyHandle body;
    ShapeHandle shape;
};

// The stable names the ECS registers these under, and the ABI and the scene format spell. The same
// arrangement `cy::scene` uses: a literal in one place, so a rename is a compile error at every use
// rather than a string that silently stops matching.
inline constexpr const char* kRigidBodyComponentName = "cy::physics::RigidBody";
inline constexpr const char* kStaticBodyComponentName = "cy::physics::StaticBody";
inline constexpr const char* kKinematicBodyComponentName = "cy::physics::KinematicBody";
inline constexpr const char* kColliderComponentName = "cy::physics::Collider";
inline constexpr const char* kTriggerComponentName = "cy::physics::Trigger";
inline constexpr const char* kPhysicsMaterialComponentName = "cy::physics::PhysicsMaterial";
inline constexpr const char* kJointComponentName = "cy::physics::Joint";
inline constexpr const char* kCharacterBodyComponentName = "cy::physics::CharacterBody";

// Chunk storage is memset and relocated by memcpy. A component that stopped being trivially
// copyable would compile everywhere and corrupt on the first archetype move, so the property is
// asserted here rather than remembered.
static_assert(std::is_trivially_copyable_v<RigidBody>);
static_assert(std::is_trivially_copyable_v<StaticBody>);
static_assert(std::is_trivially_copyable_v<KinematicBody>);
static_assert(std::is_trivially_copyable_v<Collider>);
static_assert(std::is_trivially_copyable_v<Trigger>);
static_assert(std::is_trivially_copyable_v<PhysicsMaterial>);
static_assert(std::is_trivially_copyable_v<Joint>);
static_assert(std::is_trivially_copyable_v<CharacterBody>);

/// Fill a `BodyDescription` from the components an entity carries.
///
/// One function rather than three near-identical ones in the bridge, and it is here rather than in
/// the bridge because it is the mapping from the component vocabulary to the server's — which is
/// the thing that must not differ between the ECS bridge, the scene cooker and the ABI.
[[nodiscard]] BodyDescription body_from(const RigidBody& rigid, const Transform& transform,
                                        Span<const ColliderDescription> colliders,
                                        UserData user_data) noexcept;

[[nodiscard]] BodyDescription static_body_from(const Transform& transform,
                                               Span<const ColliderDescription> colliders,
                                               UserData user_data) noexcept;

[[nodiscard]] BodyDescription kinematic_body_from(const Transform& transform,
                                                  Span<const ColliderDescription> colliders,
                                                  UserData user_data) noexcept;

}  // namespace cy::physics
