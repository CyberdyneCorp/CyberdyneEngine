// Physics' components, declared to the state hash. See cy/physics/state_schema.h, which carries
// the reasoning for every classification below.

#include <cy/physics/state_schema.h>

#include <cstddef>

namespace cy::physics {
namespace {

using determinism::SchemaSubject;
using determinism::SimulationClass;
using determinism::StateEncoding;
using determinism::StateField;

using reflect::FieldKind;

/// A handle field: one `u64`, `Derived`. Four components carry one and the argument is identical in
/// each, so it is written once.
[[nodiscard]] constexpr StateField handle_field(const char* name, u64 id, u32 offset) noexcept {
    return StateField{
        name, id, offset, FieldKind::U64, SimulationClass::Derived, StateEncoding::Direct};
}

[[nodiscard]] constexpr StateField f32_field(const char* name, u64 id, u32 offset) noexcept {
    return StateField{
        name, id, offset, FieldKind::F32, SimulationClass::Authoritative, StateEncoding::Direct};
}

[[nodiscard]] constexpr StateField bool_field(const char* name, u64 id, u32 offset) noexcept {
    return StateField{
        name, id, offset, FieldKind::Bool, SimulationClass::Authoritative, StateEncoding::Direct};
}

/// The three fields of a `ShapeDescription` an author sets and a solver reads, at a base offset.
///
/// `radius`, `half_height` and `half_extents` cover every analytic shape physics has — sphere,
/// capsule, cylinder, box and plane — and they are what a box authored as 1x2x0.5 differs from a
/// box authored as 1x2x0.6 in. The pointers are not here; the header says why at length.
[[nodiscard]] Status declare_shape_fields(StateField* out, u32& count, u32 base,
                                          u64 first_id) noexcept {
    // `FieldKind::U8` AND NOT `FieldKind::Enum`, for every enumeration below. `hash_field` carries
    // no width for `Enum` — a reflected descriptor holds it and a schema records the kind alone —
    // so it asserts on one, in as many words: "declared with the integer kind of their underlying
    // type". `ShapeType`, `CombineMode` and `ConstraintType` are all `: u8`.
    out[count++] = StateField{"shape.type",
                              first_id,
                              base + static_cast<u32>(offsetof(ShapeDescription, type)),
                              FieldKind::U8,
                              SimulationClass::Authoritative,
                              StateEncoding::Direct};
    out[count++] = f32_field("shape.radius", first_id + 1,
                             base + static_cast<u32>(offsetof(ShapeDescription, radius)));
    out[count++] = f32_field("shape.half_height", first_id + 2,
                             base + static_cast<u32>(offsetof(ShapeDescription, half_height)));
    out[count++] = f32_field("shape.half_extents.x", first_id + 3,
                             base + static_cast<u32>(offsetof(ShapeDescription, half_extents)));
    out[count++] =
        f32_field("shape.half_extents.y", first_id + 4,
                  base + static_cast<u32>(offsetof(ShapeDescription, half_extents)) + 4U);
    out[count++] =
        f32_field("shape.half_extents.z", first_id + 5,
                  base + static_cast<u32>(offsetof(ShapeDescription, half_extents)) + 8U);
    out[count++] = f32_field("shape.density", first_id + 6,
                             base + static_cast<u32>(offsetof(ShapeDescription, density)));
    return ok();
}

/// A `CollisionFilter`'s two fields at a base offset. Which layer a body is on and what it collides
/// with changes which contacts exist, so both are authoritative.
void declare_filter_fields(StateField* out, u32& count, u32 base, u64 first_id) noexcept {
    out[count++] = StateField{"filter.layer",
                              first_id,
                              base + static_cast<u32>(offsetof(CollisionFilter, layer)),
                              FieldKind::U8,
                              SimulationClass::Authoritative,
                              StateEncoding::Direct};
    out[count++] = StateField{"filter.mask",
                              first_id + 1,
                              base + static_cast<u32>(offsetof(CollisionFilter, mask)),
                              FieldKind::U32,
                              SimulationClass::Authoritative,
                              StateEncoding::Direct};
}

/// A `Transform`'s ten floats at a base offset. The same ten `scene::LocalTransform` folds, and the
/// same argument: a collider's placement on its body is authored state.
void declare_transform_fields(StateField* out, u32& count, u32 base, u64 first_id) noexcept {
    static constexpr const char* kNames[10] = {
        "local.rotation.x",    "local.rotation.y",    "local.rotation.z",    "local.rotation.w",
        "local.translation.x", "local.translation.y", "local.translation.z", "local.scale.x",
        "local.scale.y",       "local.scale.z"};
    for (u32 lane = 0; lane < 10; ++lane) {
        out[count++] = f32_field(kNames[lane], first_id + lane, base + (lane * 4U));
    }
}

}  // namespace

Status declare_physics_state(determinism::StateSchema& schema,
                             const PhysicsComponents& components) noexcept {
    if (!components.registered()) {
        return fail(ErrorCode::InvalidArgument,
                    "physics' components are not registered in this world; a schema over an "
                    "invalid component id would address nothing while reporting coverage");
    }

    // --- RigidBody ------------------------------------------------------------------------
    const StateField rigid_fields[] = {
        f32_field("mass", 1, offsetof(RigidBody, mass)),
        f32_field("center_of_mass.x", 2, offsetof(RigidBody, center_of_mass)),
        f32_field("center_of_mass.y", 3, offsetof(RigidBody, center_of_mass) + 4U),
        f32_field("center_of_mass.z", 4, offsetof(RigidBody, center_of_mass) + 8U),
        bool_field("override_center_of_mass", 5, offsetof(RigidBody, override_center_of_mass)),
        f32_field("linear_damping", 6, offsetof(RigidBody, linear_damping)),
        f32_field("angular_damping", 7, offsetof(RigidBody, angular_damping)),
        f32_field("gravity_scale", 8, offsetof(RigidBody, gravity_scale)),
        bool_field("allow_sleeping", 9, offsetof(RigidBody, allow_sleeping)),
        bool_field("start_asleep", 10, offsetof(RigidBody, start_asleep)),
        bool_field("continuous", 11, offsetof(RigidBody, continuous)),
        StateField{"locked_axes", 12, offsetof(RigidBody, locked_axes), FieldKind::U8,
                   SimulationClass::Authoritative, StateEncoding::Direct},
        handle_field("body", 13, offsetof(RigidBody, body)),
    };
    if (Status declared =
            schema.declare(SchemaSubject{components.rigid_body}, kRigidBodyComponentName,
                           Span<const StateField>(rigid_fields, 13));
        !declared) {
        return declared;
    }

    // --- StaticBody and KinematicBody -------------------------------------------------------
    //
    // A handle each, and nothing else to carry: what a static body IS is its placement and its
    // colliders, and both are other components. Declared with their one derived field, so the
    // subject is present and the report can say it folds nothing — which is a different fact from
    // "not declared", and the distinction is the whole reason `hashed_field_count` exists.
    const StateField static_fields[] = {handle_field("body", 1, offsetof(StaticBody, body))};
    if (Status declared =
            schema.declare(SchemaSubject{components.static_body}, kStaticBodyComponentName,
                           Span<const StateField>(static_fields, 1));
        !declared) {
        return declared;
    }
    const StateField kinematic_fields[] = {handle_field("body", 1, offsetof(KinematicBody, body))};
    if (Status declared =
            schema.declare(SchemaSubject{components.kinematic_body}, kKinematicBodyComponentName,
                           Span<const StateField>(kinematic_fields, 1));
        !declared) {
        return declared;
    }

    // --- Collider -------------------------------------------------------------------------
    StateField collider_fields[24];
    u32 collider_count = 0;
    if (Status declared = declare_shape_fields(collider_fields, collider_count,
                                               static_cast<u32>(offsetof(Collider, shape)), 1);
        !declared) {
        return declared;
    }
    declare_transform_fields(collider_fields, collider_count,
                             static_cast<u32>(offsetof(Collider, local)), 10);
    declare_filter_fields(collider_fields, collider_count,
                          static_cast<u32>(offsetof(Collider, filter)), 30);
    collider_fields[collider_count++] =
        f32_field("contact_impulse_threshold", 40, offsetof(Collider, contact_impulse_threshold));
    collider_fields[collider_count++] =
        bool_field("report_stay", 41, offsetof(Collider, report_stay));
    collider_fields[collider_count++] = handle_field("material", 42, offsetof(Collider, material));
    collider_fields[collider_count++] = handle_field("handle", 43, offsetof(Collider, handle));
    if (Status declared = schema.declare(SchemaSubject{components.collider}, kColliderComponentName,
                                         Span<const StateField>(collider_fields, collider_count));
        !declared) {
        return declared;
    }

    // --- Trigger --------------------------------------------------------------------------
    StateField trigger_fields[24];
    u32 trigger_count = 0;
    if (Status declared = declare_shape_fields(trigger_fields, trigger_count,
                                               static_cast<u32>(offsetof(Trigger, shape)), 1);
        !declared) {
        return declared;
    }
    declare_transform_fields(trigger_fields, trigger_count,
                             static_cast<u32>(offsetof(Trigger, local)), 10);
    declare_filter_fields(trigger_fields, trigger_count,
                          static_cast<u32>(offsetof(Trigger, filter)), 30);
    trigger_fields[trigger_count++] = handle_field("handle", 40, offsetof(Trigger, handle));
    if (Status declared = schema.declare(SchemaSubject{components.trigger}, kTriggerComponentName,
                                         Span<const StateField>(trigger_fields, trigger_count));
        !declared) {
        return declared;
    }

    // --- PhysicsMaterial --------------------------------------------------------------------
    const StateField material_fields[] = {
        f32_field("friction", 1, offsetof(PhysicsMaterial, friction)),
        f32_field("restitution", 2, offsetof(PhysicsMaterial, restitution)),
        StateField{"friction_combine", 3, offsetof(PhysicsMaterial, friction_combine),
                   FieldKind::U8, SimulationClass::Authoritative, StateEncoding::Direct},
        StateField{"restitution_combine", 4, offsetof(PhysicsMaterial, restitution_combine),
                   FieldKind::U8, SimulationClass::Authoritative, StateEncoding::Direct},
        f32_field("density", 5, offsetof(PhysicsMaterial, density)),
        handle_field("handle", 6, offsetof(PhysicsMaterial, handle)),
    };
    if (Status declared =
            schema.declare(SchemaSubject{components.material}, kPhysicsMaterialComponentName,
                           Span<const StateField>(material_fields, 6));
        !declared) {
        return declared;
    }

    // --- Joint ----------------------------------------------------------------------------
    //
    // The kind and the two bodies it joins, plus the handle. The limits and motors inside
    // `ConstraintDescription` are a tagged union of ten joint kinds; declaring them field by field
    // would be declaring nine sets of bytes that are not the active one for any given joint, and
    // the hash would then depend on padding. The kind and the endpoints are what a divergence in a
    // joint shows up as first, and neither backend creates one yet (bridge.h says so).
    const StateField joint_fields[] = {
        StateField{"kind", 1,
                   static_cast<u32>(offsetof(Joint, description)) +
                       static_cast<u32>(offsetof(ConstraintDescription, type)),
                   FieldKind::U8, SimulationClass::Authoritative, StateEncoding::Direct},
        handle_field("body_a", 2,
                     static_cast<u32>(offsetof(Joint, description)) +
                         static_cast<u32>(offsetof(ConstraintDescription, body_a))),
        handle_field("body_b", 3,
                     static_cast<u32>(offsetof(Joint, description)) +
                         static_cast<u32>(offsetof(ConstraintDescription, body_b))),
        handle_field("handle", 4, offsetof(Joint, handle)),
    };
    if (Status declared = schema.declare(SchemaSubject{components.joint}, kJointComponentName,
                                         Span<const StateField>(joint_fields, 4));
        !declared) {
        return declared;
    }

    // --- CharacterBody ----------------------------------------------------------------------
    const StateField character_fields[] = {
        f32_field("radius", 1,
                  static_cast<u32>(offsetof(CharacterBody, description)) +
                      static_cast<u32>(offsetof(CharacterDescription, radius))),
        f32_field("height", 2,
                  static_cast<u32>(offsetof(CharacterBody, description)) +
                      static_cast<u32>(offsetof(CharacterDescription, height))),
        f32_field("step_offset", 3,
                  static_cast<u32>(offsetof(CharacterBody, description)) +
                      static_cast<u32>(offsetof(CharacterDescription, step_offset))),
        f32_field("max_slope_radians", 4,
                  static_cast<u32>(offsetof(CharacterBody, description)) +
                      static_cast<u32>(offsetof(CharacterDescription, max_slope_radians))),
        handle_field("body", 5, offsetof(CharacterBody, body)),
        handle_field("shape", 6, offsetof(CharacterBody, shape)),
    };
    return schema.declare(SchemaSubject{components.character_body}, kCharacterBodyComponentName,
                          Span<const StateField>(character_fields, 6));
}

}  // namespace cy::physics
