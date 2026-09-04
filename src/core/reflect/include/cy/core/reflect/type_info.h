// Type and field metadata: what the registry stores and what generated code fills in. Task 1.1.1.
//
// A TypeInfo describes a reflected type — name, size, alignment, whether it is trivially
// relocatable, its construction and destruction thunks, its fields, its attributes, and (from M2)
// its methods. The descriptors are constexpr data in a generated translation unit; nothing here
// allocates, and nothing here is written at run time.
//
// The type it describes is untouched. Reflection is opt-in by declaration, not by inheritance, so a
// reflected component keeps its layout, its `sizeof`, and its usability in packed chunk storage —
// tests/test_registry.cpp asserts exactly that against an unannotated twin of the same struct.
//
// **Reflection is control plane.** Every lookup here — by FieldId, by name — is on the tooling,
// serialization and setup path, and each one reports itself to control_plane.h so that a lookup on
// a per-frame path is counted rather than merely discouraged. The per-frame path uses
// TypedAccessor, which is resolved once and holds a byte offset: it carries no pointer into the
// registry and it cannot perform a lookup, which is the structural half of task 1.1.4.
//
// **The two find_field() overloads below are scans, and they are the one-shot spelling.** The M2
// spec delta requires that field lookup not be linear in the field count, and `FieldIndex`
// (field_index.h) is where that requirement is met: built once per type, constant-time thereafter,
// and already built for every registered type by `TypeRegistry::fields()`. These remain because a
// caller who resolves one binding once — resolve_field(), a diagnostic, a unit test — should not
// have to build a table to read one descriptor. Anything that looks a field up more than once for
// the same type holds a FieldIndex instead; `read_record()` does, and that is the difference
// between a scene load being linear in its data and quadratic in it.

#ifndef CY_CORE_REFLECT_TYPE_INFO_H
#define CY_CORE_REFLECT_TYPE_INFO_H

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/reflect/attributes.h>
#include <cy/core/reflect/ids.h>

namespace cy::reflect {

/// The storage class of a field, which is what the serializer switches on. Deliberately small: it
/// covers what a reflected M1 type can hold. `Var`, strings and containers arrive with the values
/// module (task 1.3.x) and extend this enumeration rather than reinterpreting it — the numbers are
/// persistent because they are written into serialized records.
enum class FieldKind : u8 {
    Unsupported = 0,
    Bool = 1,
    I8 = 2,
    I16 = 3,
    I32 = 4,
    I64 = 5,
    U8 = 6,
    U16 = 7,
    U32 = 8,
    U64 = 9,
    F32 = 10,
    F64 = 11,
    Enum = 12,   ///< An enumeration; `size` gives its underlying width.
    Flags = 13,  ///< A bit set over an enumeration.
};

/// The enumerator's own spelling, for a diagnostic. Never null.
const char* field_kind_name(FieldKind kind) noexcept;

/// True when the kind is a fixed-width scalar the serializer can copy by size. Every kind at M1 is;
/// the predicate exists so that the first kind that is not fails loudly rather than silently.
[[nodiscard]] constexpr bool is_scalar(FieldKind kind) noexcept {
    return kind != FieldKind::Unsupported;
}

struct FieldInfo {
    const char* name = "";  ///< Metadata. `id` is the contract; this is what a human calls it.
    FieldId id;
    FieldKind kind = FieldKind::Unsupported;

    /// The TypeId of the field's own type when that type is itself reflected; invalid otherwise.
    TypeId type;

    /// The byte offset within the object. Available for native access and **never serialized**:
    /// layout is a compiler artefact, so serialized data addresses fields by `id`.
    u32 offset = 0;
    u32 size = 0;

    FieldAttributes attributes{};
};

using ConstructThunk = void (*)(void* storage);
using DestructThunk = void (*)(void* object);
using CopyAssignThunk = void (*)(void* destination, const void* source);

/// A reflected method. Empty at M1 — `core-type-system` calls method descriptors optional, and
/// nothing consumes them until the scripting boundary at M4. The member is present so that adding
/// one is a generator change rather than a change to every consumer of TypeInfo.
struct MethodInfo {
    const char* name = "";
    FieldId id;
};

struct TypeInfo {
    /// Fully qualified, as declared: "cy::demo::Health". Metadata — moving the type between
    /// namespaces changes this string and nothing else.
    const char* name = "";
    TypeId id;

    u32 size = 0;
    u32 alignment = 0;

    /// True when moving the object's bytes is a valid move. The ECS relies on it for chunk
    /// compaction, which is why it is recorded rather than assumed.
    bool trivially_relocatable = false;

    ConstructThunk construct = nullptr;
    DestructThunk destruct = nullptr;
    CopyAssignThunk copy_assign = nullptr;

    const FieldInfo* fields = nullptr;
    u32 field_count = 0;

    const MethodInfo* methods = nullptr;
    u32 method_count = 0;

    /// The declaring header, relative to the source root. Relative because an absolute path would
    /// make the generated output differ between checkouts, and generation must be reproducible.
    const char* header = "";

    /// The module that declared it, as the manifest records it.
    const char* module = "";

    /// The field with this identifier, or null. A reflected lookup: control plane only.
    ///
    /// **Linear in the field count.** For one lookup that is the cheapest thing there is; for a
    /// lookup that repeats, build a FieldIndex (field_index.h) or ask the registry for the one it
    /// already built. The header comment above says which is which.
    [[nodiscard]] const FieldInfo* find_field(FieldId wanted) const noexcept;

    /// The field with this name, or null. Names are metadata, so this is for tooling and
    /// diagnostics — never for anything that persists a reference. Linear, with the same caveat.
    [[nodiscard]] const FieldInfo* find_field(const char* wanted) const noexcept;
};

/// A field resolved once, addressed thereafter by offset.
///
/// This is what `core-type-system` means by "the binding SHALL be resolved to a direct accessor
/// once, and sampling SHALL not perform a reflection lookup per frame". The accessor holds an
/// offset and a size and nothing else — no registry pointer, no FieldId lookup, no dispatch — so a
/// per-frame path that holds one *cannot* reach reflection through it.
template <typename T>
class TypedAccessor {
public:
    constexpr TypedAccessor() noexcept = default;
    explicit constexpr TypedAccessor(u32 offset) noexcept : offset_(offset), bound_(true) {}

    [[nodiscard]] constexpr bool bound() const noexcept { return bound_; }
    [[nodiscard]] constexpr u32 offset() const noexcept { return offset_; }

    [[nodiscard]] T& operator()(void* object) const noexcept {
        CY_ASSERT_MSG(bound_, "TypedAccessor used before it was resolved");
        return *reinterpret_cast<T*>(static_cast<u8*>(object) + offset_);
    }
    [[nodiscard]] const T& operator()(const void* object) const noexcept {
        CY_ASSERT_MSG(bound_, "TypedAccessor used before it was resolved");
        return *reinterpret_cast<const T*>(static_cast<const u8*>(object) + offset_);
    }

private:
    u32 offset_ = 0;
    bool bound_ = false;
};

/// The FieldKind a C++ type maps to, so that resolve_field() can check a field against the type the
/// caller intends to read it as. A type with no mapping yields Unsupported and the resolve fails.
template <typename T>
struct FieldKindOf {
    static constexpr FieldKind value = FieldKind::Unsupported;
};

// clang-format off
template <> struct FieldKindOf<bool> { static constexpr FieldKind value = FieldKind::Bool; };
template <> struct FieldKindOf<i8>   { static constexpr FieldKind value = FieldKind::I8;   };
template <> struct FieldKindOf<i16>  { static constexpr FieldKind value = FieldKind::I16;  };
template <> struct FieldKindOf<i32>  { static constexpr FieldKind value = FieldKind::I32;  };
template <> struct FieldKindOf<i64>  { static constexpr FieldKind value = FieldKind::I64;  };
template <> struct FieldKindOf<u8>   { static constexpr FieldKind value = FieldKind::U8;   };
template <> struct FieldKindOf<u16>  { static constexpr FieldKind value = FieldKind::U16;  };
template <> struct FieldKindOf<u32>  { static constexpr FieldKind value = FieldKind::U32;  };
template <> struct FieldKindOf<u64>  { static constexpr FieldKind value = FieldKind::U64;  };
template <> struct FieldKindOf<f32>  { static constexpr FieldKind value = FieldKind::F32;  };
template <> struct FieldKindOf<f64>  { static constexpr FieldKind value = FieldKind::F64;  };
// clang-format on

/// Resolve a field to a typed accessor. Setup-time work: it performs one reflected lookup, checks
/// that the field really holds a T, and hands back something that never looks anything up again.
template <typename T>
[[nodiscard]] Expected<TypedAccessor<T>, Error> resolve_field(const TypeInfo& type, FieldId id) {
    const FieldInfo* field = type.find_field(id);
    if (field == nullptr) {
        return fail(ErrorCode::NotFound, "no field with that FieldId on this type");
    }
    if (field->kind != FieldKindOf<T>::value || field->size != sizeof(T)) {
        return fail(ErrorCode::InvalidArgument, "field does not hold the requested type");
    }
    return TypedAccessor<T>(field->offset);
}

}  // namespace cy::reflect

#endif  // CY_CORE_REFLECT_TYPE_INFO_H
