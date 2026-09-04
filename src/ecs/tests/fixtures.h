#pragma once
// The component set the ECS suites are written against.
//
// WHY THESE DESCRIPTORS ARE HAND-WRITTEN. A `reflect::TypeInfo` is plain constexpr data, and the
// generator emits exactly this shape — so a suite can declare one directly. It has to, at M2: the
// reflection generator's annotated-header list lives in src/core/reflect/CMakeLists.txt and the
// identifiers come from identity/manifest.toml, neither of which src/ecs/ owns this milestone. The
// two committed reflected types (`cy::demo::Health`, `cy::demo::Placement`) are used where a real
// generated descriptor is what is being tested; everything else is here.
//
// THE IDENTIFIERS BELOW ARE NOT MANIFEST IDENTIFIERS. They are never registered into
// `reflect::default_registry()` and never written to disk by anything committed; they exist only
// inside a test's own `ComponentRegistry`, which is per world. The range starts at 9000 so that a
// number here is obviously not one the manifest issued.

#include <cy/core/reflect/type_info.h>
#include <cy/ecs/world.h>

namespace cy::ecs::test {

struct Position {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 z = 0.0F;
};

struct Velocity {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 z = 0.0F;
};

/// A tag: zero fields, presence only. `sizeof` is 1 in C++ and 0 in the chunk, which is the whole
/// point of the kind.
struct Frozen {};

/// A shared component's value. Entities sharing one group into the same chunks.
struct Material {
    u32 id = 0;
};

/// A sparse component: rare, and toggled often enough that an archetype change per toggle would be
/// the dominant cost.
struct Selected {
    u64 tick = 0;
};

/// A component holding an entity reference. Its offset is declared at registration, which is how
/// serialization finds it without asking reflection per row.
struct Target {
    Entity entity;
};

/// One element of a buffer component.
struct Waypoint {
    f32 x = 0.0F;
    f32 y = 0.0F;
};

/// A descriptor of the shape generated code emits. Trivially relocatable, because every component
/// must be: chunk compaction moves a row's bytes and nothing else.
template <class T>
[[nodiscard]] inline const reflect::TypeInfo& descriptor(const char* name, u32 id) noexcept {
    static reflect::TypeInfo info;
    info.name = name;
    info.id = reflect::TypeId(id);
    info.size = static_cast<u32>(sizeof(T));
    info.alignment = static_cast<u32>(alignof(T));
    info.trivially_relocatable = true;
    return info;
}

[[nodiscard]] inline const reflect::TypeInfo& position_type() noexcept {
    return descriptor<Position>("cy::ecs::test::Position", 9001);
}
[[nodiscard]] inline const reflect::TypeInfo& velocity_type() noexcept {
    return descriptor<Velocity>("cy::ecs::test::Velocity", 9002);
}
[[nodiscard]] inline const reflect::TypeInfo& frozen_type() noexcept {
    return descriptor<Frozen>("cy::ecs::test::Frozen", 9003);
}
[[nodiscard]] inline const reflect::TypeInfo& material_type() noexcept {
    return descriptor<Material>("cy::ecs::test::Material", 9004);
}
[[nodiscard]] inline const reflect::TypeInfo& selected_type() noexcept {
    return descriptor<Selected>("cy::ecs::test::Selected", 9005);
}
[[nodiscard]] inline const reflect::TypeInfo& target_type() noexcept {
    return descriptor<Target>("cy::ecs::test::Target", 9006);
}
[[nodiscard]] inline const reflect::TypeInfo& waypoints_type() noexcept {
    return descriptor<Waypoint>("cy::ecs::test::Waypoints", 9007);
}

/// Compare two C strings by content. `CY_CHECK_EQ` on two `const char*` compares pointers, which
/// happens to pass when the linker merges two identical literals and fails when it does not —
/// AddressSanitizer's build is one where it does not.
[[nodiscard]] inline bool same_text(const char* left, const char* right) noexcept {
    if (left == nullptr || right == nullptr) {
        return left == right;
    }
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

/// The archetype an entity is in, or `kInvalidArchetype`. A helper rather than a dereference at
/// every call site: `World::location` returns null for a stale id, and -Wnull-dereference is an
/// error in this tree — correctly, because a test that crashes instead of failing reports nothing.
[[nodiscard]] inline u32 archetype_of(const World& world, Entity entity) noexcept {
    const EntityLocation* location = world.location(entity);
    return (location == nullptr) ? kInvalidArchetype : location->archetype;
}

[[nodiscard]] inline u32 row_of(const World& world, Entity entity) noexcept {
    const EntityLocation* location = world.location(entity);
    return (location == nullptr) ? 0xFFFF'FFFFu : location->row;
}

/// A component's value, or a zeroed one when the entity does not have it. A missing component then
/// shows up as a failed comparison rather than as a null dereference.
template <class T>
[[nodiscard]] inline T value_of(const World& world, Entity entity,
                                ComponentTypeId component) noexcept {
    const auto* value = world.get<T>(entity, component);
    return (value == nullptr) ? T{} : *value;
}

/// The component set most suites register, in one place so that a component id means the same thing
/// in every test that uses this struct.
struct Components {
    ComponentTypeId position = kInvalidComponent;
    ComponentTypeId velocity = kInvalidComponent;
    ComponentTypeId frozen = kInvalidComponent;
    ComponentTypeId material = kInvalidComponent;
    ComponentTypeId selected = kInvalidComponent;
    ComponentTypeId target = kInvalidComponent;
    ComponentTypeId waypoints = kInvalidComponent;
};

/// The declared offset of `Target::entity`. One entry, and it is what makes a `Target` remappable.
inline constexpr u32 kTargetEntityOffsets[] = {0};

[[nodiscard]] inline Expected<Components, Error> register_all(World& world) noexcept {
    Components ids;

    Expected<ComponentTypeId, Error> position =
        world.components().register_reflected(position_type());
    if (!position) {
        return make_unexpected(position.error());
    }
    ids.position = *position;

    Expected<ComponentTypeId, Error> velocity =
        world.components().register_reflected(velocity_type());
    if (!velocity) {
        return make_unexpected(velocity.error());
    }
    ids.velocity = *velocity;

    ComponentOptions tag;
    tag.kind = ComponentKind::Tag;
    Expected<ComponentTypeId, Error> frozen =
        world.components().register_reflected(frozen_type(), tag);
    if (!frozen) {
        return make_unexpected(frozen.error());
    }
    ids.frozen = *frozen;

    ComponentOptions shared;
    shared.kind = ComponentKind::Shared;
    Expected<ComponentTypeId, Error> material =
        world.components().register_reflected(material_type(), shared);
    if (!material) {
        return make_unexpected(material.error());
    }
    ids.material = *material;

    ComponentOptions sparse;
    sparse.kind = ComponentKind::Sparse;
    Expected<ComponentTypeId, Error> selected =
        world.components().register_reflected(selected_type(), sparse);
    if (!selected) {
        return make_unexpected(selected.error());
    }
    ids.selected = *selected;

    ComponentOptions reference;
    reference.entity_offsets = Span<const u32>(kTargetEntityOffsets, 1);
    Expected<ComponentTypeId, Error> target =
        world.components().register_reflected(target_type(), reference);
    if (!target) {
        return make_unexpected(target.error());
    }
    ids.target = *target;

    Expected<ComponentTypeId, Error> waypoints =
        world.register_buffer_component<Waypoint>(waypoints_type(), 4);
    if (!waypoints) {
        return make_unexpected(waypoints.error());
    }
    ids.waypoints = *waypoints;

    return ids;
}

}  // namespace cy::ecs::test
