// The scene's built-in components, declared to the state hash. See cy/scene/state_schema.h.

#include <cy/scene/state_schema.h>

#include <cstddef>

namespace cy::scene {
namespace {

using determinism::SchemaSubject;
using determinism::SimulationClass;
using determinism::StateEncoding;
using determinism::StateField;

using reflect::FieldKind;

/// Field ids are per subject, start at 1, and are literals rather than derived from anything. They
/// are not manifest identifiers — a built-in has none — and `StateField::id` only has to be stable
/// across runs and unique within its subject.
///
/// A transform is ten of them, because the hash reads values and never bytes: mixing `sizeof
/// (Transform)` bytes would fold the padding between `Quat` and the two `Vec3`s into the result,
/// and padding is whatever the last write to that memory happened to leave there.
constexpr u32 kQuatX = offsetof(Transform, rotation) + offsetof(Quat, x);
constexpr u32 kQuatY = offsetof(Transform, rotation) + offsetof(Quat, y);
constexpr u32 kQuatZ = offsetof(Transform, rotation) + offsetof(Quat, z);
constexpr u32 kQuatW = offsetof(Transform, rotation) + offsetof(Quat, w);
constexpr u32 kTranslationX = offsetof(Transform, translation) + offsetof(Vec3, x);
constexpr u32 kTranslationY = offsetof(Transform, translation) + offsetof(Vec3, y);
constexpr u32 kTranslationZ = offsetof(Transform, translation) + offsetof(Vec3, z);
constexpr u32 kScaleX = offsetof(Transform, scale) + offsetof(Vec3, x);
constexpr u32 kScaleY = offsetof(Transform, scale) + offsetof(Vec3, y);
constexpr u32 kScaleZ = offsetof(Transform, scale) + offsetof(Vec3, z);

/// The ten fields of a transform-valued component, at `base` within the component.
///
/// Written into a caller-owned array rather than returned, because `StateSchema::declare` takes a
/// span and the storage has to outlive the call; a function returning an array of ten `StateField`s
/// would be the same bytes with a copy in the middle.
void transform_fields(StateField (&out)[10], u32 base, SimulationClass classification) noexcept {
    struct Component {
        const char* name;
        u32 offset;
    };
    // Rotation, translation, scale — the order the struct declares them in, which is also the order
    // the hash folds them. The order is part of the hash's value, so it is written once here.
    constexpr Component kComponents[] = {
        {"rotation.x", kQuatX},
        {"rotation.y", kQuatY},
        {"rotation.z", kQuatZ},
        {"rotation.w", kQuatW},
        {"translation.x", kTranslationX},
        {"translation.y", kTranslationY},
        {"translation.z", kTranslationZ},
        {"scale.x", kScaleX},
        {"scale.y", kScaleY},
        {"scale.z", kScaleZ},
    };
    for (u32 index = 0; index < 10; ++index) {
        out[index] =
            StateField{kComponents[index].name, index + 1U,     base + kComponents[index].offset,
                       FieldKind::F32,          classification, StateEncoding::Direct};
    }
}

/// Declare a subject that has no fields to fold: a tag, or a component whose only honest answer is
/// "nothing". See the header for why that is different from not declaring it.
[[nodiscard]] Status declare_empty(determinism::StateSchema& schema, ComponentTypeId component,
                                   const char* name) noexcept {
    return schema.declare(SchemaSubject{component}, name, Span<const StateField>());
}

[[nodiscard]] Status declare_transform(determinism::StateSchema& schema, ComponentTypeId component,
                                       const char* name, u32 base,
                                       SimulationClass classification) noexcept {
    StateField fields[10];
    transform_fields(fields, base, classification);
    return schema.declare(SchemaSubject{component}, name, Span<const StateField>(fields, 10));
}

/// One interned name, hashed as its text. The one encoding that is not `Direct`, and the reason it
/// exists: a `Name`'s index is interning order and is not stable across runs.
[[nodiscard]] Status declare_name(determinism::StateSchema& schema, ComponentTypeId component,
                                  const char* name, u32 offset) noexcept {
    const StateField fields[] = {
        StateField{"value", 1, offset, FieldKind::U32, SimulationClass::Authoritative,
                   StateEncoding::InternedName},
    };
    return schema.declare(SchemaSubject{component}, name, Span<const StateField>(fields, 1));
}

[[nodiscard]] Status declare_u32(determinism::StateSchema& schema, ComponentTypeId component,
                                 const char* name, u32 offset,
                                 SimulationClass classification) noexcept {
    const StateField fields[] = {
        StateField{"value", 1, offset, FieldKind::U32, classification, StateEncoding::Direct},
    };
    return schema.declare(SchemaSubject{component}, name, Span<const StateField>(fields, 1));
}

}  // namespace

Status declare_scene_state(determinism::StateSchema& schema,
                           const SceneComponents& components) noexcept {
    if (components.node_name == kInvalidComponent) {
        return fail(ErrorCode::InvalidArgument,
                    "the scene components are not registered in this world; a schema over an "
                    "invalid component id would address nothing");
    }

    if (Status declared = declare_name(schema, components.node_name, kNodeNameComponentName,
                                       offsetof(NodeName, value));
        !declared) {
        return declared;
    }
    if (Status declared = declare_name(schema, components.node_alias, kNodeAliasComponentName,
                                       offsetof(NodeAlias, value));
        !declared) {
        return declared;
    }
    if (Status declared = declare_u32(schema, components.child_order, kChildOrderComponentName,
                                      offsetof(ChildOrder, value), SimulationClass::Authoritative);
        !declared) {
        return declared;
    }
    if (Status declared =
            declare_transform(schema, components.local_transform, kLocalTransformComponentName,
                              offsetof(LocalTransform, value), SimulationClass::Authoritative);
        !declared) {
        return declared;
    }
    if (Status declared =
            declare_transform(schema, components.world_transform, kWorldTransformComponentName,
                              offsetof(WorldTransform, value), SimulationClass::Derived);
        !declared) {
        return declared;
    }
    if (Status declared = declare_transform(
            schema, components.interpolated_transform, kInterpolatedTransformComponentName,
            offsetof(InterpolatedTransform, previous), SimulationClass::Presentation);
        !declared) {
        return declared;
    }

    // The two authored flags, as two fields rather than one: a divergence report that says which
    // flag differs is worth two entries in a table nobody walks per frame.
    const StateField flag_fields[] = {
        StateField{"visible", 1, offsetof(NodeFlags, visible), FieldKind::Bool,
                   SimulationClass::Authoritative, StateEncoding::Direct},
        StateField{"enabled", 2, offsetof(NodeFlags, enabled), FieldKind::Bool,
                   SimulationClass::Authoritative, StateEncoding::Direct},
    };
    if (Status declared = schema.declare(SchemaSubject{components.flags}, kNodeFlagsComponentName,
                                         Span<const StateField>(flag_fields, 2));
        !declared) {
        return declared;
    }

    const StateField state_fields[] = {
        StateField{"dirty", 1, offsetof(NodeState, dirty), FieldKind::U8, SimulationClass::Derived,
                   StateEncoding::Direct},
    };
    if (Status declared = schema.declare(SchemaSubject{components.state}, kNodeStateComponentName,
                                         Span<const StateField>(state_fields, 1));
        !declared) {
        return declared;
    }

    if (Status declared = declare_u32(schema, components.scene_ref, kSceneRefComponentName,
                                      offsetof(SceneRef, scene), SimulationClass::Authoritative);
        !declared) {
        return declared;
    }
    if (Status declared =
            declare_u32(schema, components.behaviour_ref, kBehaviourRefComponentName,
                        offsetof(BehaviourRef, instance), SimulationClass::Authoritative);
        !declared) {
        return declared;
    }

    // The two tags. Their presence is in the hash through the archetype key; there is nothing at a
    // row to read, and `hash_entity` skips a subject with no hashed fields before it tries.
    if (Status declared = declare_empty(schema, components.hidden, kHiddenComponentName);
        !declared) {
        return declared;
    }
    return declare_empty(schema, components.disabled, kDisabledComponentName);
}

}  // namespace cy::scene
