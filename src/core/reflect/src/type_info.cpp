// Field lookup and kind names. Task 1.1.1.
//
// Both lookups are linear and both report themselves to the control-plane counter. Linear because
// a reflected type has a handful of fields and this is not a hot path; reported because the only
// way to know it is not on a hot path is to check.

#include <cy/core/reflect/type_info.h>

#include <cy/core/reflect/control_plane.h>

namespace cy::reflect {
namespace {

bool same_string(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

}  // namespace

const char* field_kind_name(FieldKind kind) noexcept {
    switch (kind) {
        case FieldKind::Unsupported:
            return "unsupported";
        case FieldKind::Bool:
            return "bool";
        case FieldKind::I8:
            return "i8";
        case FieldKind::I16:
            return "i16";
        case FieldKind::I32:
            return "i32";
        case FieldKind::I64:
            return "i64";
        case FieldKind::U8:
            return "u8";
        case FieldKind::U16:
            return "u16";
        case FieldKind::U32:
            return "u32";
        case FieldKind::U64:
            return "u64";
        case FieldKind::F32:
            return "f32";
        case FieldKind::F64:
            return "f64";
        case FieldKind::Enum:
            return "enum";
        case FieldKind::Flags:
            return "flags";
    }
    return "unknown";
}

const FieldInfo* TypeInfo::find_field(FieldId wanted) const noexcept {
    detail::note_reflected_lookup();
    for (u32 index = 0; index < field_count; ++index) {
        if (fields[index].id == wanted) {
            return &fields[index];
        }
    }
    return nullptr;
}

const FieldInfo* TypeInfo::find_field(const char* wanted) const noexcept {
    detail::note_reflected_lookup();
    for (u32 index = 0; index < field_count; ++index) {
        if (same_string(fields[index].name, wanted)) {
            return &fields[index];
        }
    }
    return nullptr;
}

}  // namespace cy::reflect
