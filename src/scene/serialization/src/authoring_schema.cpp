// The authoring schema, built from the type registry and written as the manifest the editor reads.
// M6 task 2.4. The header carries the argument; this file carries the rules.

#include <cy/core/serialize/text.h>
#include <cy/scene/serialization/authoring_schema.h>

#include <utility>

namespace cy::scene::serialization {
namespace {

using serialize::TextWriter;

/// The one declared alias, and the whole of it. See the header for why it is declared rather than
/// derived.
struct Alias {
    std::string_view reflected;
    std::string_view authoring;
};

constexpr Alias kAliases[] = {
    // The ECS calls it the local half of a transform pair. A designer calls it the transform.
    {"cy::scene::LocalTransform", "Transform"},
};

/// The name after the last `::`, which is what a person calls a type.
[[nodiscard]] std::string_view unqualified(std::string_view name) noexcept {
    const usize separator = name.rfind("::");
    return separator == std::string_view::npos ? name : name.substr(separator + 2);
}

/// The dotted prefix of a field name, and its final component.
///
/// `value.translation.x` splits into `value.translation` and `x`. A name with no dot has an empty
/// prefix and is its own lane, which is what makes an ungrouped scalar fall through every rule
/// below unchanged.
struct Split {
    std::string_view prefix;
    std::string_view lane;
};

[[nodiscard]] Split split_lane(std::string_view name) noexcept {
    const usize dot = name.rfind('.');
    if (dot == std::string_view::npos) {
        return Split{std::string_view{}, name};
    }
    return Split{name.substr(0, dot), name.substr(dot + 1)};
}

/// The last dotted component of a prefix: `value.translation` names the field `translation`.
[[nodiscard]] std::string_view leaf_of(std::string_view prefix) noexcept {
    const usize dot = prefix.rfind('.');
    return dot == std::string_view::npos ? prefix : prefix.substr(dot + 1);
}

[[nodiscard]] bool is_float(const reflect::FieldInfo& field) noexcept {
    return field.kind == reflect::FieldKind::F32;
}

/// How many consecutive `f32` lanes starting at `first` share a prefix and spell `x`, `y`, `z`,
/// `w`.
///
/// Returns 0 when the run is not a vector: anything else — a missing lane, a lane out of order, a
/// non-float among them — is left as separate scalars rather than assembled into a value whose
/// components would then be in whatever order the generator emitted them.
[[nodiscard]] u32 vector_lanes(const reflect::TypeInfo& type, u32 first) noexcept {
    static constexpr std::string_view kLanes[] = {"x", "y", "z", "w"};
    const Split head = split_lane(type.fields[first].name);
    if (head.prefix.empty() || head.lane != kLanes[0] || !is_float(type.fields[first])) {
        return 0;
    }
    u32 count = 1;
    while (count < 4 && first + count < type.field_count) {
        const reflect::FieldInfo& candidate = type.fields[first + count];
        const Split split = split_lane(candidate.name);
        if (!is_float(candidate) || split.prefix != head.prefix || split.lane != kLanes[count]) {
            break;
        }
        ++count;
    }
    return count >= 2 ? count : 0;
}

/// The kind a run of `count` float lanes named `prefix` becomes.
///
/// Four lanes are a quaternion when the field is called a rotation and a `Vec4` otherwise. That is
/// the one place a NAME decides a type here, and it is unavoidable: `x, y, z, w` is the same ten
/// bytes either way, and only the field's own name says whether they compose an orientation. The
/// editor's gizmo needs the distinction — it slerps a `Quat` and would linearly interpolate a
/// `Vec4`.
[[nodiscard]] AuthoringKind vector_kind(u32 count, std::string_view name) noexcept {
    switch (count) {
        case 2:
            return AuthoringKind::Vec2;
        case 3:
            return AuthoringKind::Vec3;
        default:
            return name == "rotation" ? AuthoringKind::Quat : AuthoringKind::Vec4;
    }
}

/// The kind of one ungrouped reflected field.
[[nodiscard]] AuthoringKind scalar_kind(reflect::FieldKind kind) noexcept {
    switch (kind) {
        case reflect::FieldKind::Bool:
            return AuthoringKind::Bool;
        case reflect::FieldKind::F32:
            return AuthoringKind::Float;
        case reflect::FieldKind::F64:
            return AuthoringKind::Double;
        case reflect::FieldKind::Unsupported:
            // A field reflection cannot describe is carried as opaque bytes rather than dropped.
            // Losing it would be worse than not interpreting it, which is the same position
            // `wire_type_of` takes.
            return AuthoringKind::Bytes;
        default:
            // Every integer width collapses onto one editor value, exactly as they collapse onto
            // the ABI's `i64`: the storage width is a property of the field and the engine is what
            // enforces it on a write.
            return AuthoringKind::Int;
    }
}

/// A field's tooltip, which is the only description reflection carries.
[[nodiscard]] std::string_view description_of(const reflect::FieldInfo& field) noexcept {
    if (!field.attributes.declares(reflect::AttributeKind::Tooltip)) {
        return std::string_view{};
    }
    return field.attributes.tooltip.text == nullptr ? std::string_view{}
                                                    : field.attributes.tooltip.text;
}

/// Whether a field belongs in an authoring schema at all.
///
/// `Derived` is "computed; never serialised; recomputed on load", and `Transient` and `Hidden` say
/// the same thing in the two other vocabularies. A derived field in the editor's schema would be a
/// value a person could type into and that the next frame would overwrite.
[[nodiscard]] bool is_authorable(const reflect::FieldInfo& field) noexcept {
    return !field.attributes.transient() && !field.attributes.hidden() &&
           field.attributes.persistence != reflect::PersistenceKind::Derived;
}

[[nodiscard]] Status add_fields(const reflect::TypeInfo& type, AuthoringType& out) noexcept {
    for (u32 index = 0; index < type.field_count;) {
        const reflect::FieldInfo& field = type.fields[index];
        if (!is_authorable(field)) {
            ++index;
            continue;
        }
        const u32 lanes = vector_lanes(type, index);
        const Split split = split_lane(field.name);
        AuthoringField authored;
        authored.id = field.id;
        if (lanes == 0) {
            authored.kind = scalar_kind(field.kind);
            authored.name = split.prefix.empty() ? field.name : leaf_of(field.name);
        } else {
            const std::string_view name = leaf_of(split.prefix);
            authored.kind = vector_kind(lanes, name);
            authored.name = name;
        }
        authored.description = description_of(field);
        if (Status added = out.fields.push_back(authored); !added) {
            return added;
        }
        index += lanes == 0 ? 1 : lanes;
    }
    return ok();
}

[[nodiscard]] Status write_type_line(TextWriter& writer, const AuthoringType& type) noexcept {
    if (Status opened = writer.begin_line(0); !opened) {
        return opened;
    }
    if (Status written = writer.word("type"); !written) {
        return written;
    }
    if (Status written = writer.word_u64(type.id.value()); !written) {
        return written;
    }
    if (Status written = writer.word(type.authoring_only ? "authoring" : "runtime"); !written) {
        return written;
    }
    if (Status written = writer.word_quoted(type.name); !written) {
        return written;
    }
    return writer.end_line();
}

[[nodiscard]] Status write_field_line(TextWriter& writer, const AuthoringField& field) noexcept {
    if (Status opened = writer.begin_line(1); !opened) {
        return opened;
    }
    if (Status written = writer.word("field"); !written) {
        return written;
    }
    if (Status written = writer.word_u64(field.id.value()); !written) {
        return written;
    }
    if (Status written = writer.word(authoring_kind_name(field.kind)); !written) {
        return written;
    }
    if (Status written = writer.word_quoted(field.name); !written) {
        return written;
    }
    if (Status written = writer.word_quoted(field.description); !written) {
        return written;
    }
    return writer.end_line();
}

}  // namespace

const char* authoring_kind_name(AuthoringKind kind) noexcept {
    switch (kind) {
        case AuthoringKind::Bool:
            return "bool";
        case AuthoringKind::Int:
            return "int";
        case AuthoringKind::Float:
            return "float";
        case AuthoringKind::Double:
            return "double";
        case AuthoringKind::Vec2:
            return "vec2";
        case AuthoringKind::Vec3:
            return "vec3";
        case AuthoringKind::Vec4:
            return "vec4";
        case AuthoringKind::Quat:
            return "quat";
        case AuthoringKind::Text:
            return "text";
        case AuthoringKind::Bytes:
            return "bytes";
        case AuthoringKind::Entity:
            return "entity";
    }
    return "bytes";
}

std::string_view authoring_name_of(std::string_view reflected_name) noexcept {
    for (const Alias& alias : kAliases) {
        if (alias.reflected == reflected_name) {
            return alias.authoring;
        }
    }
    return unqualified(reflected_name);
}

Status build_authoring_schema(const reflect::TypeRegistry& registry,
                              AuthoringSchema& out) noexcept {
    out.types().clear();
    if (Status reserved = out.types().reserve(registry.size()); !reserved) {
        return reserved;
    }
    for (const reflect::TypeInfo* entry : registry) {
        const reflect::TypeInfo& type = *entry;
        AuthoringType authored(out.allocator());
        authored.id = type.id;
        authored.name = authoring_name_of(type.name);
        // Nothing in the engine's registry is authoring-only: an authoring-only type is one the
        // EDITOR declares — a note, a folder, a selection marker — and it reaches a document from
        // the editor's own schema rather than from here. Written explicitly so the manifest carries
        // the answer rather than leaving the reader to default it.
        authored.authoring_only = false;
        if (Status added = add_fields(type, authored); !added) {
            return added;
        }
        if (Status pushed = out.types().push_back(std::move(authored)); !pushed) {
            return pushed;
        }
    }
    // ORDERED BY IDENTIFIER, BECAUSE A REGISTRY'S ORDER IS ITS HASH TABLE'S. Two processes that
    // registered the same types in a different order iterate them differently, and a manifest whose
    // line order depended on that would fail its own regeneration gate on another machine.
    // Insertion sort over a few dozen entries, which is the whole of the type registry.
    for (usize index = 1; index < out.types().size(); ++index) {
        for (usize back = index;
             back > 0 && out.types()[back].id.value() < out.types()[back - 1].id.value(); --back) {
            AuthoringType moved = std::move(out.types()[back]);
            out.types()[back] = std::move(out.types()[back - 1]);
            out.types()[back - 1] = std::move(moved);
        }
    }
    return ok();
}

Status write_authoring_schema(const AuthoringSchema& schema, Array<char>& out) noexcept {
    TextWriter writer(out);
    if (Status opened = writer.begin_line(0); !opened) {
        return opened;
    }
    if (Status written = writer.word(kAuthoringSchemaMagic); !written) {
        return written;
    }
    if (Status written = writer.word_u64(kAuthoringSchemaVersion); !written) {
        return written;
    }
    if (Status closed = writer.end_line(); !closed) {
        return closed;
    }

    for (const AuthoringType& type : schema.types()) {
        if (Status written = write_type_line(writer, type); !written) {
            return written;
        }
        for (const AuthoringField& field : type.fields) {
            if (Status written = write_field_line(writer, field); !written) {
                return written;
            }
        }
    }
    return ok();
}

}  // namespace cy::scene::serialization
