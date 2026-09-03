// Field attributes as strongly typed structures. Task 1.1.3.
//
// `core-type-system` is explicit that these are "strongly typed structures, not a string-keyed
// dictionary of string values, so that the generator validates them at build time and consumers
// read typed data". Everything here is therefore a struct with named members of the right type, and
// every one of them is filled in by generated code from an annotation the generator has already
// checked. A malformed attribute — a Range whose minimum exceeds its maximum, an unknown name, a
// wrong argument count — fails generation naming the field, so no consumer here has to defend
// against one.
//
// The payloads point at string literals in the generated translation unit, which outlives every
// reader. Nothing here owns storage, and a FieldAttributes is trivially copyable, so a descriptor
// can sit in a constexpr array.
//
// A module that needs an attribute the table does not have declares its own in an attribute schema
// (tools/gen/attributes/), and the generator emits it as a typed struct exactly like these ones —
// reached through find_custom<T>() below rather than by parsing a string.

#ifndef CY_CORE_REFLECT_ATTRIBUTES_H
#define CY_CORE_REFLECT_ATTRIBUTES_H

#include <cy/core/base/types.h>

namespace cy::reflect {

/// Which attributes a field actually declared. A payload member is only meaningful when its bit is
/// set: an absent Range is not "a Range of zero to zero".
enum class AttributeKind : u32 {
    None = 0,
    Range = 1u << 0,
    Enum = 1u << 1,
    Flags = 1u << 2,
    Hidden = 1u << 3,
    ReadOnly = 1u << 4,
    Category = 1u << 5,
    Tooltip = 1u << 6,
    Transient = 1u << 7,
    Replicated = 1u << 8,
    AssetRef = 1u << 9,
    Unit = 1u << 10,
    Persistence = 1u << 11,
};

constexpr AttributeKind operator|(AttributeKind a, AttributeKind b) noexcept {
    return static_cast<AttributeKind>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr AttributeKind& operator|=(AttributeKind& a, AttributeKind b) noexcept {
    a = a | b;
    return a;
}
constexpr bool has(AttributeKind set, AttributeKind one) noexcept {
    return (static_cast<u32>(set) & static_cast<u32>(one)) != 0;
}

/// Metres, radians, seconds — for display and for conversion. Carried as a unit rather than as a
/// naming convention, because "is this angle degrees?" is not answerable from a field name.
enum class UnitKind : u8 {
    None = 0,
    Metres,
    Radians,
    Degrees,
    Seconds,
    Milliseconds,
    Kilograms,
    Newtons,
    Percent,
};

/// The field classification `serialization-and-prefabs` defines. One declaration from which
/// serialization, live update, persistence and networking all derive their behaviour.
enum class PersistenceKind : u8 {
    Authoring = 0,  ///< Defined by the asset; updated when the asset changes.
    RuntimeState,   ///< Owned by the running simulation; preserved across asset updates, not saved.
    PersistentState,  ///< Owned by the simulation and written to the persistence overlay.
    Derived,          ///< Computed; never serialised; recomputed on load and on change.
};

/// A bounded numeric field. `step` is zero when the annotation named no step.
struct RangeAttribute {
    f64 minimum = 0.0;
    f64 maximum = 0.0;
    f64 step = 0.0;
};

/// One name/value pair of an Enum or Flags attribute. The value is persistent: it is written to
/// serialized data, so reordering the enumerators does not change it.
struct Enumerator {
    const char* name = "";
    i64 value = 0;
};

/// The enumerators of an Enum or Flags field, in declaration order.
struct EnumAttribute {
    const Enumerator* values = nullptr;
    u32 count = 0;
};

struct TextAttribute {
    const char* text = "";
};

/// Replication: which encoder, its parameters, and the condition under which the field is sent.
/// All three are opaque to this module — `networking-and-replication` gives them meaning.
struct ReplicatedAttribute {
    const char* encoder = "";
    const char* parameters = "";
    const char* condition = "";
};

/// The asset kind a field references, so the inspector offers the right picker without knowing the
/// field's type.
struct AssetRefAttribute {
    const char* kind = "";
};

/// One attribute a module declared for itself. `value` points at a generated struct of exactly
/// `size` bytes whose definition the generator emitted next to it; find_custom<T>() checks the size
/// before handing back a T, so a schema that changed under a consumer is a null rather than a
/// misread.
struct CustomAttribute {
    const char* name = "";
    const void* value = nullptr;
    u32 size = 0;
};

/// Everything one field declared. Trivially copyable, no ownership, safe in a constexpr array.
struct FieldAttributes {
    AttributeKind declared = AttributeKind::None;

    RangeAttribute range{};
    EnumAttribute enumeration{};
    TextAttribute category{};
    TextAttribute tooltip{};
    ReplicatedAttribute replicated{};
    AssetRefAttribute asset_ref{};
    UnitKind unit = UnitKind::None;
    PersistenceKind persistence = PersistenceKind::Authoring;

    const CustomAttribute* custom = nullptr;
    u32 custom_count = 0;

    [[nodiscard]] constexpr bool declares(AttributeKind kind) const noexcept {
        return has(declared, kind);
    }

    /// Excluded from serialization and from replication. Asked often enough to deserve a name.
    [[nodiscard]] constexpr bool transient() const noexcept {
        return declares(AttributeKind::Transient);
    }
    [[nodiscard]] constexpr bool hidden() const noexcept { return declares(AttributeKind::Hidden); }
    [[nodiscard]] constexpr bool read_only() const noexcept {
        return declares(AttributeKind::ReadOnly);
    }
};

/// The declared value of a module's own attribute, or null when this field does not carry it or
/// when the generated struct is not the T the caller expects.
///
/// The size check is the whole safety argument: a project that changes its attribute schema and
/// rebuilds gets a null here rather than a reinterpretation of unrelated bytes. Consumers of a
/// custom attribute are control-plane code by construction, so the branch costs nothing that
/// matters.
template <typename T>
const T* find_custom(const FieldAttributes& attributes, const char* name) noexcept {
    for (u32 index = 0; index < attributes.custom_count; ++index) {
        const CustomAttribute& candidate = attributes.custom[index];
        if (candidate.size != sizeof(T)) {
            continue;
        }
        const char* a = candidate.name;
        const char* b = name;
        while (*a != '\0' && *a == *b) {
            ++a;
            ++b;
        }
        if (*a == '\0' && *b == '\0') {
            return static_cast<const T*>(candidate.value);
        }
    }
    return nullptr;
}

/// The enumerator's own spelling, for a diagnostic and for the inspector. Never null.
const char* unit_name(UnitKind unit) noexcept;
const char* persistence_name(PersistenceKind persistence) noexcept;

}  // namespace cy::reflect

#endif  // CY_CORE_REFLECT_ATTRIBUTES_H
