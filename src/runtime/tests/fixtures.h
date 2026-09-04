#pragma once
// The world the runtime's suites are written against: two reflected components with real fields, so
// that the state hash has something to cover and a divergence has a field to name.
//
// WHY THESE DESCRIPTORS ARE HAND-WRITTEN. The same reason src/ecs/tests/fixtures.h gives: a
// `reflect::TypeInfo` is plain constexpr data and the generator emits exactly this shape, but the
// generator's annotated-header list lives in src/core/reflect/CMakeLists.txt and the identifiers in
// identity/manifest.toml, neither of which src/runtime/ owns this milestone.
//
// THE IDENTIFIERS BELOW ARE NOT MANIFEST IDENTIFIERS. They are never registered into
// `reflect::default_registry()` and never written to disk; they exist only inside a test's own
// `ComponentRegistry`, which is per world. The range starts at 9100 so that a number here is
// obviously not one the manifest issued, and does not collide with the ECS suite's 9000s.

#include <cy/core/reflect/type_info.h>
#include <cy/ecs/world.h>

namespace cy::runtime::test {

/// Authoritative position. Deliberately padded after `alive`, so that "raw memory is not the hash"
/// is a claim these suites can falsify rather than assert.
struct Position {
    f32 x = 0.0F;
    f32 y = 0.0F;
    bool alive = true;
    // three bytes of padding
    u32 revision = 0;
};

/// A component whose one field is presentation-classified, so that a world can be changed without
/// its hash changing.
struct Flash {
    f32 intensity = 0.0F;
};

inline const reflect::FieldInfo* position_fields() noexcept {
    static const reflect::FieldInfo fields[] = {
        [] {
            reflect::FieldInfo field;
            field.name = "x";
            field.id = reflect::FieldId(1);
            field.kind = reflect::FieldKind::F32;
            field.offset = offsetof(Position, x);
            field.size = sizeof(f32);
            field.attributes.persistence = reflect::PersistenceKind::RuntimeState;
            return field;
        }(),
        [] {
            reflect::FieldInfo field;
            field.name = "y";
            field.id = reflect::FieldId(2);
            field.kind = reflect::FieldKind::F32;
            field.offset = offsetof(Position, y);
            field.size = sizeof(f32);
            field.attributes.persistence = reflect::PersistenceKind::RuntimeState;
            return field;
        }(),
        [] {
            reflect::FieldInfo field;
            field.name = "alive";
            field.id = reflect::FieldId(3);
            field.kind = reflect::FieldKind::Bool;
            field.offset = offsetof(Position, alive);
            field.size = sizeof(bool);
            field.attributes.persistence = reflect::PersistenceKind::PersistentState;
            return field;
        }(),
        [] {
            // Derived: recomputed, never hashed. Present so that a schema derived from reflection
            // is visibly narrower than the struct.
            reflect::FieldInfo field;
            field.name = "revision";
            field.id = reflect::FieldId(4);
            field.kind = reflect::FieldKind::U32;
            field.offset = offsetof(Position, revision);
            field.size = sizeof(u32);
            field.attributes.persistence = reflect::PersistenceKind::Derived;
            return field;
        }(),
    };
    return fields;
}

inline const reflect::FieldInfo* flash_fields() noexcept {
    static const reflect::FieldInfo fields[] = {[] {
        reflect::FieldInfo field;
        field.name = "intensity";
        field.id = reflect::FieldId(1);
        field.kind = reflect::FieldKind::F32;
        field.offset = offsetof(Flash, intensity);
        field.size = sizeof(f32);
        field.attributes.persistence = reflect::PersistenceKind::Derived;
        return field;
    }()};
    return fields;
}

inline const reflect::TypeInfo& position_type() noexcept {
    static const reflect::TypeInfo info = [] {
        reflect::TypeInfo type;
        type.name = "cy::runtime::test::Position";
        type.id = reflect::TypeId(9101);
        type.size = sizeof(Position);
        type.alignment = alignof(Position);
        type.trivially_relocatable = true;
        type.fields = position_fields();
        type.field_count = 4;
        return type;
    }();
    return info;
}

inline const reflect::TypeInfo& flash_type() noexcept {
    static const reflect::TypeInfo info = [] {
        reflect::TypeInfo type;
        type.name = "cy::runtime::test::Flash";
        type.id = reflect::TypeId(9102);
        type.size = sizeof(Flash);
        type.alignment = alignof(Flash);
        type.trivially_relocatable = true;
        type.fields = flash_fields();
        type.field_count = 1;
        return type;
    }();
    return info;
}

}  // namespace cy::runtime::test
