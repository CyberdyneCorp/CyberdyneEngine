#pragma once
// The reflected types the serialization suites are written against.
//
// WHY THESE DESCRIPTORS ARE HAND-WRITTEN. A `reflect::TypeInfo` is plain constexpr data and the
// generator emits exactly this shape, so a suite can declare one directly — and at M2 it has to.
// The generator's annotated-header list lives in src/core/reflect/CMakeLists.txt and the
// identifiers come from identity/manifest.toml, neither of which this module owns this milestone.
// src/ecs/'s fixtures record the same seam for the same reason.
//
// THE IDENTIFIERS BELOW ARE NOT MANIFEST IDENTIFIERS. Nothing here is registered into
// `reflect::default_registry()` and nothing here is written to any committed file. The range starts
// at 9100 so that a number in this file is visibly not one the manifest issued, and so that it does
// not collide with src/ecs/'s fixtures at 9000 if the two ever meet in one process.

#include <cy/core/base/types.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/attributes.h>
#include <cy/core/reflect/type_info.h>

#include <cstddef>

namespace cy::serialize::test {

/// The allocator every suite here builds its containers from. Serialization is authoring and asset
/// work, so it is attributed to the assets domain rather than to the engine root — the budget tree
/// exists to say where memory went, and "somewhere" is not an answer.
[[nodiscard]] inline Allocator& test_allocator() noexcept {
    return system_allocator(MemoryDomain::Assets);
}

/// The type every round-trip case uses. One field of each class, so that the classification table
/// is exercised by the same object that exercises the encoding.
struct Health {
    f32 maximum = 100.0F;   ///< Authoring: the asset defines it.
    f32 current = 100.0F;   ///< RuntimeState: the simulation owns it, and a save does not carry it.
    u32 revives = 0;        ///< PersistentState: written to the persistence overlay.
    f32 fraction = 1.0F;    ///< Derived: never serialised, recomputed on load.
    u64 debug_counter = 0;  ///< Transient: excluded from serialization and replication entirely.
};

inline constexpr u32 kHealthTypeId = 9101;
inline constexpr u32 kHealthMaximum = 1;
inline constexpr u32 kHealthCurrent = 2;
inline constexpr u32 kHealthRevives = 3;
inline constexpr u32 kHealthFraction = 4;
inline constexpr u32 kHealthDebugCounter = 5;

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

[[nodiscard]] inline const reflect::TypeInfo& health_type() noexcept {
    static const reflect::FieldInfo fields[] = {
        make_field("maximum", kHealthMaximum, reflect::FieldKind::F32,
                   static_cast<u32>(offsetof(Health, maximum)), sizeof(f32),
                   reflect::PersistenceKind::Authoring),
        make_field("current", kHealthCurrent, reflect::FieldKind::F32,
                   static_cast<u32>(offsetof(Health, current)), sizeof(f32),
                   reflect::PersistenceKind::RuntimeState),
        make_field("revives", kHealthRevives, reflect::FieldKind::U32,
                   static_cast<u32>(offsetof(Health, revives)), sizeof(u32),
                   reflect::PersistenceKind::PersistentState),
        make_field("fraction", kHealthFraction, reflect::FieldKind::F32,
                   static_cast<u32>(offsetof(Health, fraction)), sizeof(f32),
                   reflect::PersistenceKind::Derived),
        make_field("debug_counter", kHealthDebugCounter, reflect::FieldKind::U64,
                   static_cast<u32>(offsetof(Health, debug_counter)), sizeof(u64),
                   reflect::PersistenceKind::Authoring, /*transient=*/true),
    };
    static reflect::TypeInfo info;
    info.name = "cy::serialize::test::Health";
    info.id = reflect::TypeId(kHealthTypeId);
    info.size = static_cast<u32>(sizeof(Health));
    info.alignment = static_cast<u32>(alignof(Health));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = static_cast<u32>(sizeof(fields) / sizeof(fields[0]));
    return info;
}

/// A type carrying one of every scalar width and sign, for the encoding cases.
struct Spread {
    bool flag = false;
    i8 small_signed = 0;
    i16 medium_signed = 0;
    i32 large_signed = 0;
    i64 huge_signed = 0;
    u8 small = 0;
    u16 medium = 0;
    u32 large = 0;
    u64 huge = 0;
    f32 single = 0.0F;
    f64 doubled = 0.0;
};

inline constexpr u32 kSpreadTypeId = 9102;

[[nodiscard]] inline const reflect::TypeInfo& spread_type() noexcept {
    using reflect::FieldKind;
    using reflect::PersistenceKind;
    static const reflect::FieldInfo fields[] = {
        make_field("flag", 1, FieldKind::Bool, static_cast<u32>(offsetof(Spread, flag)),
                   sizeof(bool), PersistenceKind::Authoring),
        make_field("small_signed", 2, FieldKind::I8,
                   static_cast<u32>(offsetof(Spread, small_signed)), sizeof(i8),
                   PersistenceKind::Authoring),
        make_field("medium_signed", 3, FieldKind::I16,
                   static_cast<u32>(offsetof(Spread, medium_signed)), sizeof(i16),
                   PersistenceKind::Authoring),
        make_field("large_signed", 4, FieldKind::I32,
                   static_cast<u32>(offsetof(Spread, large_signed)), sizeof(i32),
                   PersistenceKind::Authoring),
        make_field("huge_signed", 5, FieldKind::I64,
                   static_cast<u32>(offsetof(Spread, huge_signed)), sizeof(i64),
                   PersistenceKind::Authoring),
        make_field("small", 6, FieldKind::U8, static_cast<u32>(offsetof(Spread, small)), sizeof(u8),
                   PersistenceKind::Authoring),
        make_field("medium", 7, FieldKind::U16, static_cast<u32>(offsetof(Spread, medium)),
                   sizeof(u16), PersistenceKind::Authoring),
        make_field("large", 8, FieldKind::U32, static_cast<u32>(offsetof(Spread, large)),
                   sizeof(u32), PersistenceKind::Authoring),
        make_field("huge", 9, FieldKind::U64, static_cast<u32>(offsetof(Spread, huge)), sizeof(u64),
                   PersistenceKind::Authoring),
        make_field("single", 10, FieldKind::F32, static_cast<u32>(offsetof(Spread, single)),
                   sizeof(f32), PersistenceKind::Authoring),
        make_field("doubled", 11, FieldKind::F64, static_cast<u32>(offsetof(Spread, doubled)),
                   sizeof(f64), PersistenceKind::Authoring),
    };
    static reflect::TypeInfo info;
    info.name = "cy::serialize::test::Spread";
    info.id = reflect::TypeId(kSpreadTypeId);
    info.size = static_cast<u32>(sizeof(Spread));
    info.alignment = static_cast<u32>(alignof(Spread));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = static_cast<u32>(sizeof(fields) / sizeof(fields[0]));
    return info;
}

}  // namespace cy::serialize::test
