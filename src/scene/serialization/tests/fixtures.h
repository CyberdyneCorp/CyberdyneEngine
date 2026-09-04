#pragma once
// The component set and the awkward prefab the serialization suites are written against.
//
// WHY THESE DESCRIPTORS ARE HAND-WRITTEN. A `reflect::TypeInfo` is plain constexpr data and the
// generator emits exactly this shape. At M2 it has to be written here: the generator's
// annotated-header list lives in src/core/reflect/CMakeLists.txt and the identifiers come from
// identity/manifest.toml, neither of which this module owns. src/ecs/tests/fixtures.h and
// src/core/serialize/tests/fixtures.h record the same seam. Identifiers start at 9300 so that a
// number here is visibly not one the manifest issued and does not collide with either of those.
//
// TWO OF THE THREE COMPONENTS EXIST TO EXERCISE A SPECIFIC CLAIM.
//
//   `Placement` holds a `cy::Transform` as one forty-byte field, because M1's reflection has no
//   vector field kind — a `Vec3` member is an opaque run to it. That is the seam `TransformBinding`
//   is built around, and cooking composes and bakes through it.
//
//   `Target` holds an entity reference at a declared byte offset, which is the one thing that makes
//   the cooked reference-site table testable: a field is a reference precisely when its offset is
//   one the component declared.

#include <cy/core/base/types.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/attributes.h>
#include <cy/core/reflect/type_info.h>
#include <cy/ecs/world.h>
#include <cy/scene/serialization/document.h>

#include <cstddef>

namespace cy::scene::serialization::test {

/// Serialization is authoring and asset work, so its test allocations are attributed to the assets
/// domain rather than to the engine root.
[[nodiscard]] inline Allocator& test_allocator() noexcept {
    return system_allocator(MemoryDomain::Assets);
}

/// A local placement. One field, forty bytes, holding a `cy::Transform`.
struct Placement {
    cy::Transform local;
};

/// Two fields of different classification, so that live update's "update the maximum, keep the
/// current" scenario is expressible.
struct Health {
    f32 maximum = 100.0F;
    f32 current = 100.0F;
};

/// An entity reference at a declared offset, and a plain field beside it so the reference is not
/// the whole component.
struct Target {
    u64 entity = 0;
    u32 slot = 0;
};

inline constexpr u32 kPlacementType = 9301;
inline constexpr u32 kHealthType = 9302;
inline constexpr u32 kTargetType = 9303;

inline constexpr u32 kPlacementLocal = 1;
inline constexpr u32 kHealthMaximum = 1;
inline constexpr u32 kHealthCurrent = 2;
inline constexpr u32 kTargetEntity = 1;
inline constexpr u32 kTargetSlot = 2;

[[nodiscard]] inline reflect::FieldInfo make_field(
    const char* name, u32 id, reflect::FieldKind kind, u32 offset, u32 size,
    reflect::PersistenceKind persistence = reflect::PersistenceKind::Authoring) noexcept {
    reflect::FieldInfo field;
    field.name = name;
    field.id = reflect::FieldId(id);
    field.kind = kind;
    field.offset = offset;
    field.size = size;
    field.attributes.declared = reflect::AttributeKind::Persistence;
    field.attributes.persistence = persistence;
    return field;
}

[[nodiscard]] inline const reflect::TypeInfo& placement_type() noexcept {
    static const reflect::FieldInfo fields[] = {
        // `FieldKind::Unsupported` is what M1's reflection calls a run of bytes it cannot describe,
        // and it maps to `WireType::Bytes`, which round-trips exactly. That is the honest
        // description of a `cy::Transform` field today.
        make_field("local", kPlacementLocal, reflect::FieldKind::Unsupported,
                   static_cast<u32>(offsetof(Placement, local)), sizeof(cy::Transform)),
    };
    static reflect::TypeInfo info;
    info.name = "cy::scene::serialization::test::Placement";
    info.id = reflect::TypeId(kPlacementType);
    info.size = static_cast<u32>(sizeof(Placement));
    info.alignment = static_cast<u32>(alignof(Placement));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = 1;
    return info;
}

[[nodiscard]] inline const reflect::TypeInfo& health_type() noexcept {
    static const reflect::FieldInfo fields[] = {
        make_field("maximum", kHealthMaximum, reflect::FieldKind::F32,
                   static_cast<u32>(offsetof(Health, maximum)), sizeof(f32),
                   reflect::PersistenceKind::Authoring),
        make_field("current", kHealthCurrent, reflect::FieldKind::F32,
                   static_cast<u32>(offsetof(Health, current)), sizeof(f32),
                   reflect::PersistenceKind::RuntimeState),
    };
    static reflect::TypeInfo info;
    info.name = "cy::scene::serialization::test::Health";
    info.id = reflect::TypeId(kHealthType);
    info.size = static_cast<u32>(sizeof(Health));
    info.alignment = static_cast<u32>(alignof(Health));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = 2;
    return info;
}

[[nodiscard]] inline const reflect::TypeInfo& target_type() noexcept {
    static const reflect::FieldInfo fields[] = {
        make_field("entity", kTargetEntity, reflect::FieldKind::U64,
                   static_cast<u32>(offsetof(Target, entity)), sizeof(u64)),
        make_field("slot", kTargetSlot, reflect::FieldKind::U32,
                   static_cast<u32>(offsetof(Target, slot)), sizeof(u32)),
    };
    static reflect::TypeInfo info;
    info.name = "cy::scene::serialization::test::Target";
    info.id = reflect::TypeId(kTargetType);
    info.size = static_cast<u32>(sizeof(Target));
    info.alignment = static_cast<u32>(alignof(Target));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = 2;
    return info;
}

/// A world with the three fixture components registered, in a fixed order.
[[nodiscard]] inline Status register_fixture_components(ecs::World& world) noexcept {
    if (auto id = world.components().register_reflected(placement_type()); !id) {
        return make_unexpected(id.error());
    }
    if (auto id = world.components().register_reflected(health_type()); !id) {
        return make_unexpected(id.error());
    }
    const u32 offsets[] = {static_cast<u32>(offsetof(Target, entity))};
    ecs::ComponentOptions options;
    options.entity_offsets = Span<const u32>(offsets, 1);
    if (auto id = world.components().register_reflected(target_type(), options); !id) {
        return make_unexpected(id.error());
    }
    return ok();
}

/// An asset id that reads as a number in a diagnostic, so a failing test names the document.
[[nodiscard]] inline AssetId asset(u64 low) noexcept {
    return {0, low};
}

/// Write a `cy::Transform` into an entity's `Placement` component.
[[nodiscard]] inline Status set_placement(DocumentEntity& entity,
                                          const cy::Transform& transform) noexcept {
    const Expected<ComponentData*, Error> component =
        entity.ensure(reflect::TypeId(kPlacementType));
    if (!component) {
        return make_unexpected(component.error());
    }
    return (*component)
        ->record.set(reflect::FieldId(kPlacementLocal), serialize::WireType::Bytes, &transform,
                     static_cast<u32>(sizeof(transform)));
}

[[nodiscard]] inline Status set_health(DocumentEntity& entity, f32 maximum, f32 current) noexcept {
    const Expected<ComponentData*, Error> component = entity.ensure(reflect::TypeId(kHealthType));
    if (!component) {
        return make_unexpected(component.error());
    }
    if (Status written =
            (*component)
                ->record.set_scalar(reflect::FieldId(kHealthMaximum), serialize::WireType::F32,
                                    &maximum, sizeof(maximum));
        !written) {
        return written;
    }
    return (*component)
        ->record.set_scalar(reflect::FieldId(kHealthCurrent), serialize::WireType::F32, &current,
                            sizeof(current));
}

/// Point an entity's `Target` at another entity, by local id.
[[nodiscard]] inline Status set_target(DocumentEntity& entity, LocalId target) noexcept {
    const Expected<ComponentData*, Error> component = entity.ensure(reflect::TypeId(kTargetType));
    if (!component) {
        return make_unexpected(component.error());
    }
    return (*component)
        ->record.set_local_reference(reflect::FieldId(kTargetEntity), target.value());
}

}  // namespace cy::scene::serialization::test
