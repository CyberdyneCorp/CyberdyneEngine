#include <cy/core/serialize/traversal.h>

namespace cy::serialize {

const char* purpose_name(Purpose purpose) noexcept {
    switch (purpose) {
        case Purpose::Asset:
            return "Asset";
        case Purpose::Persistence:
            return "Persistence";
        case Purpose::Snapshot:
            return "Snapshot";
    }
    return "<invalid>";
}

u32 written_field_count(const reflect::TypeInfo& type, Purpose purpose) noexcept {
    u32 written = 0;
    for (u32 index = 0; index < type.field_count; ++index) {
        if (field_is_written(type.fields[index], purpose)) {
            ++written;
        }
    }
    return written;
}

Status visit_object(const reflect::TypeInfo& type, const void* object, Purpose purpose,
                    FieldVisitor& visitor) noexcept {
    if (object == nullptr) {
        return fail(ErrorCode::InvalidArgument, "cannot traverse a null object");
    }
    if (Status begun = visitor.begin_object(type, written_field_count(type, purpose)); !begun) {
        return begun;
    }

    const u8* base = static_cast<const u8*>(object);
    for (u32 index = 0; index < type.field_count; ++index) {
        const reflect::FieldInfo& field = type.fields[index];
        if (!field_is_written(field, purpose)) {
            continue;
        }
        // A field with no identifier cannot be addressed by anything that reads the result, so
        // writing it would produce a record no reader can apply. The generator cannot emit one —
        // the manifest assigns every number — so this catches a hand-written descriptor, which is
        // what the ECS's fixtures and every test in this module use.
        if (!field.id.valid()) {
            return fail(ErrorCode::InvalidArgument, "a reflected field carries no FieldId");
        }
        if (Status visited = visitor.visit_field(field, base + field.offset); !visited) {
            return visited;
        }
    }

    return visitor.end_object(type);
}

}  // namespace cy::serialize
