#pragma once
// Compiled identifiers: names, categories and classified fields.
//
// `diagnostics-profiling-and-crash` — "Trace identity and formatting": event names, categories and
// structured field names are compiled to stable identifiers, registered once, with debug names held
// in a metadata table. Nothing in the emission path formats, copies or hashes a string; a producer
// passes identifiers that were computed the first time its declaration was reached.
//
// THE INVARIANT (design.md section 2). A field's privacy classification is a required argument of
// CY_TRACE_FIELD. There is no overload that omits it, no default argument, and no second way to
// obtain a FieldId:
//
//   CY_TRACE_FIELD(frame_index, u64,    cy::Privacy::Public)
//   CY_TRACE_FIELD(user_path,   string, cy::Privacy::Sensitive)
//
// Omitting the third argument is a preprocessor error — the macro takes exactly three parameters —
// and passing something that is not a Privacy constant is a type error, because
// require_classification() is consteval and takes Privacy by value. An id that was never registered
// carries no classification, so the writer redacts it and counts it: an unclassified field is
// reported rather than exported.

#include <cy/core/diagnostics/prelude.h>
#include <cy/core/diagnostics/privacy.h>

namespace cy::diag {

using NameId = u32;
using CategoryId = u32;
using FieldId = u32;

inline constexpr NameId kInvalidName = 0;
inline constexpr CategoryId kInvalidCategory = 0;
inline constexpr FieldId kInvalidField = 0;

/// The value shapes a field may carry. Text is the only one whose payload is bytes rather than a
/// machine word, and it is the one redaction most often applies to.
///
/// The enumerators are spelled out while CY_TRACE_FIELD takes the short names design.md section 2
/// uses — `u64`, `string` — because those short names are also this module's type aliases, and an
/// enumerator that shadows a type is a warning the engine builds with -Werror.
enum class FieldType : u8 {
    UnsignedInteger = 0,
    SignedInteger = 1,
    Real = 2,
    Boolean = 3,
    Text = 4,
    Identifier = 5,
    DurationNanoseconds = 6,
    ByteCount = 7,
};

/// Register a literal name. Idempotent: the same pointer or the same text yields the same id.
/// Called once per declaration site, never in the emission path.
NameId register_name(const char* name) noexcept;
CategoryId register_category(const char* name) noexcept;

/// Register a field. Every parameter is required; there is no overload with fewer.
FieldId register_field(const char* name, FieldType type, Privacy privacy) noexcept;

/// The identity function that makes an omitted or non-constant classification a compile error.
consteval Privacy require_classification(Privacy classification) noexcept {
    return classification;
}

struct FieldInfo {
    const char* name;
    FieldType type;
    Privacy privacy;
};

/// Resolve a registered field. False when the id was never registered — which the writer treats as
/// unclassified, and therefore as not exportable at any level.
bool lookup_field(FieldId id, FieldInfo& out) noexcept;

const char* lookup_name(NameId id) noexcept;
const char* lookup_category(CategoryId id) noexcept;

/// How much of the fixed-capacity metadata table is in use, and what it refused. The tables do not
/// grow: registration allocates nothing, and an overflow is reported rather than resized into.
struct RegistryStats {
    u32 names = 0;
    u32 categories = 0;
    u32 fields = 0;
    u32 rejected = 0;
};

RegistryStats registry_stats() noexcept;

}  // namespace cy::diag

// The type vocabulary CY_TRACE_FIELD accepts. A token that is not one of these produces an
// undefined-identifier error naming CY_DIAG_FIELD_TYPE_<token>, which says what was misspelled.
#define CY_DIAG_FIELD_TYPE_u64 ::cy::diag::FieldType::UnsignedInteger
#define CY_DIAG_FIELD_TYPE_i64 ::cy::diag::FieldType::SignedInteger
#define CY_DIAG_FIELD_TYPE_f64 ::cy::diag::FieldType::Real
#define CY_DIAG_FIELD_TYPE_boolean ::cy::diag::FieldType::Boolean
#define CY_DIAG_FIELD_TYPE_string ::cy::diag::FieldType::Text
#define CY_DIAG_FIELD_TYPE_id ::cy::diag::FieldType::Identifier
#define CY_DIAG_FIELD_TYPE_duration_ns ::cy::diag::FieldType::DurationNanoseconds
#define CY_DIAG_FIELD_TYPE_bytes ::cy::diag::FieldType::ByteCount

#define CY_DIAG_JOIN_(a, b) a##b
#define CY_DIAG_JOIN(a, b) CY_DIAG_JOIN_(a, b)

/// Declare a classified field. `ident` becomes a function returning its stable id.
///
///   CY_TRACE_FIELD(frame_index, u64, cy::Privacy::Public)
///   ... trace_instant(name, category, channel, fields, count);
///
/// Three arguments, always. Two is `error: macro "CY_TRACE_FIELD" requires 3 arguments, but only 2
/// given` — the invariant, enforced by the preprocessor rather than by review.
#define CY_TRACE_FIELD(ident, field_type, classification)                                     \
    [[maybe_unused]] inline ::cy::diag::FieldId ident() noexcept {                            \
        static const ::cy::diag::FieldId id_ =                                                \
            ::cy::diag::register_field(#ident, CY_DIAG_JOIN(CY_DIAG_FIELD_TYPE_, field_type), \
                                       ::cy::diag::require_classification(classification));   \
        return id_;                                                                           \
    }

/// Declare a trace or log category. Categories are stable identifiers and extensible by plugins.
///
/// [[maybe_unused]] on each of these three: a declaration site is often an anonymous namespace, and
/// a declared-but-not-yet-used identifier is a -Wunused-function error under clang. Declaring the
/// vocabulary a subsystem will emit, before it emits all of it, is normal and is not a defect.
#define CY_TRACE_CATEGORY(ident, literal)                                                 \
    [[maybe_unused]] inline ::cy::diag::CategoryId ident() noexcept {                     \
        static const ::cy::diag::CategoryId id_ = ::cy::diag::register_category(literal); \
        return id_;                                                                       \
    }

/// Declare an event name.
#define CY_TRACE_NAME(ident, literal)                                             \
    [[maybe_unused]] inline ::cy::diag::NameId ident() noexcept {                 \
        static const ::cy::diag::NameId id_ = ::cy::diag::register_name(literal); \
        return id_;                                                               \
    }
