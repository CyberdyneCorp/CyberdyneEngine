#pragma once
// The reflected types the save suites are written against.
//
// WHY THESE DESCRIPTORS ARE HAND-WRITTEN. A `reflect::TypeInfo` is plain constexpr data and the
// generator emits exactly this shape, so a suite can declare one directly. The generator's
// annotated-header list lives in src/core/reflect/CMakeLists.txt and the identifiers come from
// identity/manifest.toml, neither of which this module owns; src/core/serialize/tests/fixtures.h
// and src/ecs/tests/fixtures.h record the same seam for the same reason.
//
// THE IDENTIFIERS BELOW ARE NOT MANIFEST IDENTIFIERS. Nothing here is registered into
// `reflect::default_registry()` and nothing here is written to any committed file. The range starts
// at 9400 so that a number in this file is visibly not one the manifest issued and does not collide
// with the ECS suite's 9000s, the serialization suite's 9100s or the scene suite's 9300s.

#include <cy/core/base/types.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/attributes.h>
#include <cy/core/reflect/type_info.h>
#include <cy/save/traits.h>

#include <cstddef>

namespace cy::save::test {

/// The allocator every suite here builds its containers from. A save is asset-and-storage work, so
/// it is attributed to the assets domain rather than to the engine root — the budget tree exists to
/// say where memory went, and "somewhere" is not an answer.
[[nodiscard]] inline Allocator& test_allocator() noexcept {
    return system_allocator(MemoryDomain::Assets);
}

/// A field descriptor of the shape generated code emits.
[[nodiscard]] inline reflect::FieldInfo make_field(const char* name, u32 id,
                                                   reflect::FieldKind kind, u32 offset, u32 size,
                                                   reflect::PersistenceKind persistence,
                                                   bool transient = false) noexcept {
    reflect::FieldInfo field;
    field.name = name;
    field.id = reflect::FieldId(id);
    field.kind = kind;
    field.offset = offset;
    field.size = size;
    field.attributes.declared = reflect::AttributeKind::Persistence;
    field.attributes.persistence = persistence;
    if (transient) {
        field.attributes.declared |= reflect::AttributeKind::Transient;
    }
    return field;
}

/// One field of each class, so the classification table is exercised by the same object that
/// exercises the encoding. `revives` is the only field a save carries.
struct Health {
    f32 maximum = 100.0F;   ///< Authoring: the asset defines it.
    f32 current = 100.0F;   ///< RuntimeState: the simulation owns it; a save does not carry it.
    u32 revives = 0;        ///< PersistentState: written to the persistence overlay.
    f32 fraction = 1.0F;    ///< Derived: never serialised, recomputed on load.
    u64 debug_counter = 0;  ///< Transient: excluded from serialization and replication entirely.
};

inline constexpr u32 kHealthTypeId = 9401;
inline constexpr u32 kHealthMaximum = 1;
inline constexpr u32 kHealthCurrent = 2;
inline constexpr u32 kHealthRevives = 3;
inline constexpr u32 kHealthFraction = 4;
inline constexpr u32 kHealthDebugCounter = 5;

[[nodiscard]] inline const reflect::TypeInfo& health_type() noexcept {
    using reflect::FieldKind;
    using reflect::PersistenceKind;
    static const reflect::FieldInfo fields[] = {
        make_field("maximum", kHealthMaximum, FieldKind::F32,
                   static_cast<u32>(offsetof(Health, maximum)), sizeof(f32),
                   PersistenceKind::Authoring),
        make_field("current", kHealthCurrent, FieldKind::F32,
                   static_cast<u32>(offsetof(Health, current)), sizeof(f32),
                   PersistenceKind::RuntimeState),
        make_field("revives", kHealthRevives, FieldKind::U32,
                   static_cast<u32>(offsetof(Health, revives)), sizeof(u32),
                   PersistenceKind::PersistentState),
        make_field("fraction", kHealthFraction, FieldKind::F32,
                   static_cast<u32>(offsetof(Health, fraction)), sizeof(f32),
                   PersistenceKind::Derived),
        make_field("debug_counter", kHealthDebugCounter, FieldKind::U64,
                   static_cast<u32>(offsetof(Health, debug_counter)), sizeof(u64),
                   PersistenceKind::Authoring, /*transient=*/true),
    };
    static reflect::TypeInfo info;
    info.name = "cy::save::test::Health";
    info.id = reflect::TypeId(kHealthTypeId);
    info.size = static_cast<u32>(sizeof(Health));
    info.alignment = static_cast<u32>(alignof(Health));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = static_cast<u32>(sizeof(fields) / sizeof(fields[0]));
    return info;
}

/// A structure that is destroyed and rebuilt: what a player-built wall carries.
struct Structure {
    u32 material = 0;   ///< PersistentState.
    u32 integrity = 0;  ///< PersistentState.
};

inline constexpr u32 kStructureTypeId = 9402;
inline constexpr u32 kStructureMaterial = 1;
inline constexpr u32 kStructureIntegrity = 2;

[[nodiscard]] inline const reflect::TypeInfo& structure_type() noexcept {
    using reflect::FieldKind;
    using reflect::PersistenceKind;
    static const reflect::FieldInfo fields[] = {
        make_field("material", kStructureMaterial, FieldKind::U32,
                   static_cast<u32>(offsetof(Structure, material)), sizeof(u32),
                   PersistenceKind::PersistentState),
        make_field("integrity", kStructureIntegrity, FieldKind::U32,
                   static_cast<u32>(offsetof(Structure, integrity)), sizeof(u32),
                   PersistenceKind::PersistentState),
    };
    static reflect::TypeInfo info;
    info.name = "cy::save::test::Structure";
    info.id = reflect::TypeId(kStructureTypeId);
    info.size = static_cast<u32>(sizeof(Structure));
    info.alignment = static_cast<u32>(alignof(Structure));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = static_cast<u32>(sizeof(fields) / sizeof(fields[0]));
    return info;
}

/// Settings: saved, and routed to the profile scope by the custom attribute the generator would
/// emit for `@SaveScope(profile)`. This is what a project's own attribute schema produces, built by
/// hand for the same reason the descriptors above are.
struct Settings {
    f32 gamma = 1.0F;
    u32 language = 0;
};

inline constexpr u32 kSettingsTypeId = 9403;
inline constexpr u32 kSettingsGamma = 1;
inline constexpr u32 kSettingsLanguage = 2;

[[nodiscard]] inline const reflect::TypeInfo& settings_type() noexcept {
    using reflect::FieldKind;
    using reflect::PersistenceKind;
    static const SaveScopeAttribute profile{static_cast<u8>(Scope::Profile)};
    static const reflect::CustomAttribute custom[] = {
        reflect::CustomAttribute{kSaveScopeAttribute, &profile, sizeof(SaveScopeAttribute)},
    };
    static reflect::FieldInfo fields[] = {
        make_field("gamma", kSettingsGamma, FieldKind::F32,
                   static_cast<u32>(offsetof(Settings, gamma)), sizeof(f32),
                   PersistenceKind::PersistentState),
        make_field("language", kSettingsLanguage, FieldKind::U32,
                   static_cast<u32>(offsetof(Settings, language)), sizeof(u32),
                   PersistenceKind::PersistentState),
    };
    for (reflect::FieldInfo& field : fields) {
        field.attributes.custom = custom;
        field.attributes.custom_count = 1;
    }
    static reflect::TypeInfo info;
    info.name = "cy::save::test::Settings";
    info.id = reflect::TypeId(kSettingsTypeId);
    info.size = static_cast<u32>(sizeof(Settings));
    info.alignment = static_cast<u32>(alignof(Settings));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = static_cast<u32>(sizeof(fields) / sizeof(fields[0]));
    return info;
}

/// Two identities that read differently in a failure message.
[[nodiscard]] inline PersistentId entity(u64 low) noexcept {
    return {0x1000'0000'0000'0000ULL, low};
}

}  // namespace cy::save::test
