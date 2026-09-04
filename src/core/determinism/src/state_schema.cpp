#include <cy/core/determinism/state_schema.h>

#include <cy/core/values/name.h>

#include <cy/core/base/assert.h>

#include <algorithm>
#include <cstring>

namespace cy::determinism {
namespace {

/// Read a scalar of `Width` bytes at `base + offset` without an unaligned load and without reading
/// anything the field does not own.
template <class T>
[[nodiscard]] T read_at(const void* base, u32 offset) noexcept {
    T value{};
    std::memcpy(&value, static_cast<const u8*>(base) + offset, sizeof(T));
    return value;
}

}  // namespace

const char* simulation_class_name(SimulationClass value) noexcept {
    switch (value) {
        case SimulationClass::Authoritative:
            return "authoritative";
        case SimulationClass::Predicted:
            return "predicted";
        case SimulationClass::Persistent:
            return "persistent";
        case SimulationClass::Presentation:
            return "presentation";
        case SimulationClass::Derived:
            return "derived";
    }
    return "unknown";
}

bool kind_is_hashable(reflect::FieldKind kind) noexcept {
    return kind != reflect::FieldKind::Unsupported;
}

void hash_field(StateHashTree& tree, const StateField& field, const void* base) noexcept {
    if (field.encoding == StateEncoding::InternedName) {
        // The text, never the index. See StateEncoding::InternedName for why that is the whole
        // point of the encoding existing.
        const Name name = Name::from_index(read_at<u32>(base, field.offset));
        tree.mix_text(name.c_str());
        return;
    }
    switch (field.kind) {
        case reflect::FieldKind::Bool:
            tree.mix_u64(read_at<bool>(base, field.offset) ? 1U : 0U);
            return;
        case reflect::FieldKind::I8:
            tree.mix_i64(read_at<i8>(base, field.offset));
            return;
        case reflect::FieldKind::I16:
            tree.mix_i64(read_at<i16>(base, field.offset));
            return;
        case reflect::FieldKind::I32:
            tree.mix_i64(read_at<i32>(base, field.offset));
            return;
        case reflect::FieldKind::I64:
            tree.mix_i64(read_at<i64>(base, field.offset));
            return;
        case reflect::FieldKind::U8:
            tree.mix_u64(read_at<u8>(base, field.offset));
            return;
        case reflect::FieldKind::U16:
            tree.mix_u64(read_at<u16>(base, field.offset));
            return;
        case reflect::FieldKind::U32:
            tree.mix_u64(read_at<u32>(base, field.offset));
            return;
        case reflect::FieldKind::U64:
            tree.mix_u64(read_at<u64>(base, field.offset));
            return;
        case reflect::FieldKind::F32:
            tree.mix_f32(read_at<f32>(base, field.offset));
            return;
        case reflect::FieldKind::F64:
            tree.mix_f64(read_at<f64>(base, field.offset));
            return;
        case reflect::FieldKind::Enum:
        case reflect::FieldKind::Flags:
        case reflect::FieldKind::Unsupported:
            break;
    }
    // Enum and Flags carry their width in the reflected descriptor rather than in the kind, and a
    // schema records the kind alone. They are declared with the integer kind of their underlying
    // type — `declare_reflected()` does that translation — so reaching here means an Unsupported
    // field slipped past `declare()`, which refuses one.
    CY_ASSERT_MSG(false, "hash_field() reached a kind the schema should have refused");
}

Status StateSchema::declare(SchemaSubject subject, const char* name,
                            Span<const StateField> fields) noexcept {
    if (frozen_) {
        return fail(ErrorCode::Unavailable,
                    "the state schema is frozen; every subject is declared before simulation "
                    "begins so that the walk order is a function of the declarations and not of "
                    "when they arrived");
    }
    if (find(subject) != nullptr) {
        return fail(ErrorCode::AlreadyExists, "this subject already has a state schema");
    }

    for (usize outer = 0; outer < fields.size(); ++outer) {
        if (!kind_is_hashable(fields[outer].kind)) {
            return fail(ErrorCode::Unsupported,
                        "a state field's kind cannot be read by value; declare it with the integer "
                        "kind of its underlying type, or leave it out of the schema");
        }
        if (fields[outer].encoding == StateEncoding::InternedName &&
            fields[outer].kind != reflect::FieldKind::U32) {
            // A `Name` is one 32-bit word and the encoding reads exactly that. Refused here rather
            // than misread at hash time, where the wrong width would produce a plausible number.
            return fail(ErrorCode::InvalidArgument,
                        "an InternedName field is a cy::Name, which is four bytes; declare it with "
                        "FieldKind::U32");
        }
        for (usize inner = outer + 1; inner < fields.size(); ++inner) {
            if (fields[outer].id == fields[inner].id) {
                return fail(ErrorCode::AlreadyExists,
                            "two fields of one subject claim the same field id; the id is what a "
                            "divergence report names, so it has to be unique");
            }
        }
    }

    SubjectSchema record;
    record.subject = subject;
    record.name = name != nullptr ? name : "";
    record.first_field = static_cast<u32>(fields_.size());
    record.field_count = static_cast<u32>(fields.size());
    for (const StateField& field : fields) {
        if (Status added = fields_.push_back(field); !added) {
            (void)fields_.resize(record.first_field);
            return added;
        }
        if (participation_of(field.classification).hashed) {
            ++record.hashed_field_count;
        }
    }

    Status added = subjects_.push_back(record);
    if (!added) {
        (void)fields_.resize(record.first_field);
    }
    return added;
}

Status StateSchema::declare_reflected(SchemaSubject subject,
                                      const reflect::TypeInfo& type) noexcept {
    Array<StateField> derived(fields_.allocator());
    if (Status reserved = derived.reserve(type.field_count); !reserved) {
        return reserved;
    }

    for (u32 index = 0; index < type.field_count; ++index) {
        const reflect::FieldInfo& info = type.fields[index];
        if (info.attributes.transient()) {
            // Declared not to be state by `core-type-system`. Excluding it here rather than giving
            // it a class keeps one rule in one place: serialize::field_is_written() excludes it for
            // the same reason.
            continue;
        }
        if (!kind_is_hashable(info.kind)) {
            continue;
        }

        StateField field;
        field.name = info.name;
        field.id = info.id.value();
        field.offset = info.offset;
        field.kind = info.kind;
        field.classification = class_of(info.attributes.persistence);

        // Enum and Flags are integers of the width the descriptor records. Translated here so that
        // `hash_field` never has to consult a `TypeInfo` it was not given.
        if (info.kind == reflect::FieldKind::Enum || info.kind == reflect::FieldKind::Flags) {
            switch (info.size) {
                case 1:
                    field.kind = reflect::FieldKind::U8;
                    break;
                case 2:
                    field.kind = reflect::FieldKind::U16;
                    break;
                case 4:
                    field.kind = reflect::FieldKind::U32;
                    break;
                case 8:
                    field.kind = reflect::FieldKind::U64;
                    break;
                default:
                    continue;
            }
        }

        if (Status pushed = derived.push_back(field); !pushed) {
            return pushed;
        }
    }

    return declare(subject, type.name, derived.span());
}

Status StateSchema::override_classification(SchemaSubject subject, u64 field_id,
                                            SimulationClass classification) noexcept {
    if (frozen_) {
        return fail(ErrorCode::Unavailable, "the state schema is frozen");
    }
    SubjectSchema* record = find_mutable(subject);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "no state schema is declared for this subject");
    }
    for (u32 offset = 0; offset < record->field_count; ++offset) {
        StateField& field = fields_[record->first_field + offset];
        if (field.id != field_id) {
            continue;
        }
        const bool was_hashed = participation_of(field.classification).hashed;
        const bool now_hashed = participation_of(classification).hashed;
        field.classification = classification;
        record->hashed_field_count += static_cast<u32>(now_hashed) - static_cast<u32>(was_hashed);
        return ok();
    }
    return fail(ErrorCode::NotFound,
                "this subject has no field with that id; a reclassification that silently did "
                "nothing would leave the field in the hash under its derived class");
}

void StateSchema::freeze() noexcept {
    // Sorted by the subject's number, which is the stable identifier a walk must not depend on the
    // declaration order of. The field array is not moved: a subject addresses its fields by index,
    // and reordering them would change nothing a consumer can observe while costing a copy.
    std::ranges::sort(subjects_, [](const SubjectSchema& a, const SubjectSchema& b) noexcept {
        return a.subject.value < b.subject.value;
    });
    frozen_ = true;
}

const SubjectSchema* StateSchema::find(SchemaSubject subject) const noexcept {
    for (const SubjectSchema& record : subjects_) {
        if (record.subject == subject) {
            return &record;
        }
    }
    return nullptr;
}

SubjectSchema* StateSchema::find_mutable(SchemaSubject subject) noexcept {
    for (SubjectSchema& record : subjects_) {
        if (record.subject == subject) {
            return &record;
        }
    }
    return nullptr;
}

Span<const StateField> StateSchema::fields_of(const SubjectSchema& subject) const noexcept {
    return {fields_.data() + subject.first_field, subject.field_count};
}

}  // namespace cy::determinism
