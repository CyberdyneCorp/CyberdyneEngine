// The canonical text form of an authoring document. Task 3.2.3.
//
// The grammar, at the indent depths shown. Every list is written in the order the document holds
// it, which is ascending by identifier, so two documents with the same content produce the same
// bytes.
//
//   cydoc <version>
//   kind <prefab|scene|world>
//   id <32 hex>
//   schema <n>
//   next <n>                      the local-id counter, so ids issued after a load do not collide
//   base <32 hex>                 a prefab variant only
//   basemap <source> <local>
//   basearg <parameter> <wire> <bytes> <value>
//   baseoverride <op> <entity> <type> <field> <parent> <schema> <conflict>
//     record <type> <schema>
//       <field> <wire> <bytes> <value>
//   parameter <id> <wire> "<name>" "<documentation>"
//     default <wire> <bytes> <value>
//     bind <entity> <type> <field>
//   entity <id> <parent> <static|dynamic> <auto|keep|flatten> "<name>"
//     record <type> <schema>
//       <field> <wire> <bytes> <value>
//   instance <id> <parent> <32 hex> <embedded|packed> "<name>"
//     transform <80 hex>          forty bytes of cy::Transform, only when it is not the identity
//     map <source> <local>
//     arg <parameter> <wire> <bytes> <value>
//     override <op> <entity> <type> <field> <parent> <schema> <conflict>
//       record <type> <schema>
//
// Field identifiers are numbers and no name appears beside them, which is the decision
// `<cy/core/serialize/text.h>` argues: a name in the file would make renaming a field rewrite every
// scene that touches it, which is the cost the identity model exists to remove.

#include <cy/core/serialize/text.h>
#include <cy/scene/serialization/format.h>

#include <utility>

namespace cy::scene::serialization {
namespace {

using serialize::TextLine;
using serialize::TextScanner;
using serialize::TextWriter;
using serialize::WireType;

constexpr usize kAssetIdHexLength = 32;
constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] Status write_hex(TextWriter& writer, const void* bytes, usize count,
                               Array<char>& scratch) noexcept {
    scratch.clear();
    const u8* cursor = static_cast<const u8*>(bytes);
    for (usize index = 0; index < count; ++index) {
        if (Status added = scratch.push_back(kHexDigits[cursor[index] >> 4U]); !added) {
            return added;
        }
        if (Status added = scratch.push_back(kHexDigits[cursor[index] & 0x0FU]); !added) {
            return added;
        }
    }
    return writer.word(std::string_view(scratch.data(), scratch.size()));
}

[[nodiscard]] Expected<u8, Error> hex_value(char character) noexcept {
    if (character >= '0' && character <= '9') {
        return static_cast<u8>(character - '0');
    }
    if (character >= 'a' && character <= 'f') {
        return static_cast<u8>(character - 'a' + 10);
    }
    return fail(ErrorCode::InvalidArgument, "expected a lowercase hexadecimal digit");
}

[[nodiscard]] Status read_hex(std::string_view text, void* out, usize count) noexcept {
    if (text.size() != count * 2) {
        return fail(ErrorCode::InvalidArgument, "hexadecimal run is not the expected length");
    }
    u8* cursor = static_cast<u8*>(out);
    for (usize index = 0; index < count; ++index) {
        const Expected<u8, Error> high = hex_value(text[index * 2]);
        if (!high) {
            return make_unexpected(high.error());
        }
        const Expected<u8, Error> low = hex_value(text[(index * 2) + 1]);
        if (!low) {
            return make_unexpected(low.error());
        }
        cursor[index] = static_cast<u8>((high.value() << 4U) | low.value());
    }
    return ok();
}

[[nodiscard]] Status write_asset_id(TextWriter& writer, AssetId id, Array<char>& scratch) noexcept {
    char text[AssetId::kTextLength + 1] = {};
    (void)id.format(text);
    (void)scratch;
    return writer.word(std::string_view(text, AssetId::kTextLength));
}

[[nodiscard]] Expected<AssetId, Error> read_asset_id(std::string_view text) noexcept {
    if (text.size() != kAssetIdHexLength) {
        return fail(ErrorCode::InvalidArgument, "an asset id is 32 hexadecimal digits");
    }
    return AssetId::parse(text);
}

[[nodiscard]] const char* motion_name(MotionKind motion) noexcept {
    return motion == MotionKind::Dynamic ? "dynamic" : "static";
}

[[nodiscard]] Expected<MotionKind, Error> motion_from_name(std::string_view text) noexcept {
    if (text == "static") {
        return MotionKind::Static;
    }
    if (text == "dynamic") {
        return MotionKind::Dynamic;
    }
    return fail(ErrorCode::InvalidArgument, "a motion is 'static' or 'dynamic'");
}

[[nodiscard]] Expected<FlattenPolicy, Error> flatten_from_name(std::string_view text) noexcept {
    for (u8 value = 0; value <= static_cast<u8>(FlattenPolicy::Flatten); ++value) {
        if (text == flatten_policy_name(static_cast<FlattenPolicy>(value))) {
            return static_cast<FlattenPolicy>(value);
        }
    }
    return fail(ErrorCode::InvalidArgument, "unknown flatten policy");
}

[[nodiscard]] Expected<CookMode, Error> cook_mode_from_name(std::string_view text) noexcept {
    for (u8 value = 0; value <= static_cast<u8>(CookMode::Packed); ++value) {
        if (text == cook_mode_name(static_cast<CookMode>(value))) {
            return static_cast<CookMode>(value);
        }
    }
    return fail(ErrorCode::InvalidArgument, "unknown cook mode");
}

[[nodiscard]] Expected<AssetKind, Error> asset_kind_from_name(std::string_view text) noexcept {
    for (u8 value = 0; value <= static_cast<u8>(AssetKind::World); ++value) {
        if (text == asset_kind_name(static_cast<AssetKind>(value))) {
            return static_cast<AssetKind>(value);
        }
    }
    return fail(ErrorCode::InvalidArgument, "unknown asset kind");
}

[[nodiscard]] Expected<OverrideOp, Error> override_op_from_name(std::string_view text) noexcept {
    for (u8 value = 0; value <= static_cast<u8>(OverrideOp::ReparentEntity); ++value) {
        if (text == override_op_name(static_cast<OverrideOp>(value))) {
            return static_cast<OverrideOp>(value);
        }
    }
    return fail(ErrorCode::InvalidArgument, "unknown override operation");
}

[[nodiscard]] Expected<ConflictKind, Error> conflict_from_name(std::string_view text) noexcept {
    for (u8 value = 0; value <= static_cast<u8>(ConflictKind::MissingParent); ++value) {
        if (text == conflict_kind_name(static_cast<ConflictKind>(value))) {
            return static_cast<ConflictKind>(value);
        }
    }
    return fail(ErrorCode::InvalidArgument, "unknown conflict kind");
}

[[nodiscard]] WireType wire_from_name(std::string_view text) noexcept {
    for (u8 value = 0; value < static_cast<u8>(WireType::Count); ++value) {
        if (text == serialize::wire_type_name(static_cast<WireType>(value))) {
            return static_cast<WireType>(value);
        }
    }
    return WireType::Count;
}

/// One `<wire> <bytes> <value>` triple, as a parameter default and an argument are written.
[[nodiscard]] Status write_blob(TextWriter& writer, Array<char>& out, WireType wire,
                                Span<const u8> bytes) noexcept {
    if (Status written = writer.word(serialize::wire_type_name(wire)); !written) {
        return written;
    }
    if (Status written = writer.word_u64(bytes.size()); !written) {
        return written;
    }
    if (Status written = writer.word(""); !written) {
        return written;
    }
    return serialize::write_value_text(out, wire, bytes);
}

[[nodiscard]] Status read_blob(const TextLine& line, usize first, WireType& wire,
                               Array<u8>& out) noexcept {
    wire = wire_from_name(line.word(first));
    if (wire == WireType::Count) {
        return fail(ErrorCode::Unsupported, "unknown wire type");
    }
    const Expected<u64, Error> size = line.word_u64(first + 1);
    if (!size) {
        return make_unexpected(size.error());
    }
    return serialize::read_value_text(line, first + 2, wire, static_cast<u32>(size.value()), out);
}

/// A `bind <entity> <type> <field>` line, one per field an exposed parameter drives.
[[nodiscard]] Status write_binding_line(TextWriter& writer,
                                        const ParameterBinding& binding) noexcept {
    if (Status opened = writer.begin_line(1); !opened) {
        return opened;
    }
    if (Status written = writer.word("bind"); !written) {
        return written;
    }
    if (Status written = writer.word_u64(binding.entity.value()); !written) {
        return written;
    }
    if (Status written = writer.word_u64(binding.component.value()); !written) {
        return written;
    }
    if (Status written = writer.word_u64(binding.field.value()); !written) {
        return written;
    }
    return writer.end_line();
}

[[nodiscard]] Status write_override(TextWriter& writer, Array<char>& out, u32 depth,
                                    const char* keyword, const Override& item) noexcept {
    if (Status opened = writer.begin_line(depth); !opened) {
        return opened;
    }
    if (Status written = writer.word(keyword); !written) {
        return written;
    }
    if (Status written = writer.word(override_op_name(item.op())); !written) {
        return written;
    }
    if (Status written = writer.word_u64(item.target().entity.value()); !written) {
        return written;
    }
    if (Status written = writer.word_u64(item.target().component.value()); !written) {
        return written;
    }
    if (Status written = writer.word_u64(item.target().field.value()); !written) {
        return written;
    }
    if (Status written = writer.word_u64(item.parent().value()); !written) {
        return written;
    }
    if (Status written = writer.word_u64(item.schema_version()); !written) {
        return written;
    }
    if (Status written = writer.word(conflict_kind_name(item.conflict())); !written) {
        return written;
    }
    if (Status closed = writer.end_line(); !closed) {
        return closed;
    }
    (void)out;
    if (item.payload().empty() && !item.payload().type().valid()) {
        return ok();
    }
    return writer.write_record(depth + 1, item.payload());
}

[[nodiscard]] Status read_override(TextScanner& scanner, const TextLine& line,
                                   OverrideList& out) noexcept {
    if (line.count() < 8) {
        return fail(ErrorCode::InvalidArgument, "an override line carries eight words");
    }
    Override item(out.allocator());

    const Expected<OverrideOp, Error> op = override_op_from_name(line.word(1));
    if (!op) {
        return make_unexpected(op.error());
    }
    item.set_op(op.value());

    OverrideTarget target;
    const Expected<u64, Error> entity = line.word_u64(2);
    const Expected<u64, Error> component = line.word_u64(3);
    const Expected<u64, Error> field = line.word_u64(4);
    const Expected<u64, Error> parent = line.word_u64(5);
    const Expected<u64, Error> schema = line.word_u64(6);
    if (!entity || !component || !field || !parent || !schema) {
        return fail(ErrorCode::InvalidArgument, "an override line's numbers are decimal");
    }
    target.entity = LocalId(static_cast<u32>(entity.value()));
    target.component = reflect::TypeId(static_cast<u32>(component.value()));
    target.field = reflect::FieldId(static_cast<u32>(field.value()));
    item.set_target(target);
    item.set_parent(LocalId(static_cast<u32>(parent.value())));
    item.set_schema_version(static_cast<u16>(schema.value()));

    const Expected<ConflictKind, Error> conflict = conflict_from_name(line.word(7));
    if (!conflict) {
        return make_unexpected(conflict.error());
    }
    item.set_conflict(conflict.value());

    const Expected<TextLine, Error> next = scanner.next();
    if (next && next->depth() > line.depth() && next->word(0) == "record") {
        if (Status read = serialize::read_record_text(scanner, next.value(), item.payload());
            !read) {
            return read;
        }
    } else if (next) {
        scanner.push_back();
    }
    return out.add(std::move(item));
}

}  // namespace

namespace {

/// A `<keyword> <number>` line at the outermost depth.
[[nodiscard]] Status write_header_line(TextWriter& writer, const char* keyword,
                                       u64 value) noexcept {
    if (Status opened = writer.begin_line(0); !opened) {
        return opened;
    }
    if (Status written = writer.word(keyword); !written) {
        return written;
    }
    if (Status written = writer.word_u64(value); !written) {
        return written;
    }
    return writer.end_line();
}

/// A `<keyword> <word>` line at the outermost depth.
[[nodiscard]] Status write_word_line(TextWriter& writer, const char* keyword,
                                     std::string_view value) noexcept {
    if (Status opened = writer.begin_line(0); !opened) {
        return opened;
    }
    if (Status written = writer.word(keyword); !written) {
        return written;
    }
    if (Status written = writer.word(value); !written) {
        return written;
    }
    return writer.end_line();
}

/// A `<keyword> <32 hex>` line at the outermost depth.
[[nodiscard]] Status write_asset_line(TextWriter& writer, const char* keyword,
                                      AssetId id) noexcept {
    char text[AssetId::kTextLength + 1] = {};
    (void)id.format(text);
    return write_word_line(writer, keyword, std::string_view(text, AssetId::kTextLength));
}

/// A `<keyword> <source> <local>` line at `depth`.
[[nodiscard]] Status write_mapping_line(TextWriter& writer, u32 depth, const char* keyword,
                                        const InstanceMapping& entry) noexcept {
    if (Status opened = writer.begin_line(depth); !opened) {
        return opened;
    }
    if (Status written = writer.word(keyword); !written) {
        return written;
    }
    if (Status written = writer.word_u64(entry.source.value()); !written) {
        return written;
    }
    if (Status written = writer.word_u64(entry.local.value()); !written) {
        return written;
    }
    return writer.end_line();
}

/// A `<keyword> <parameter> <wire> <bytes> <value>` line at `depth`.
[[nodiscard]] Status write_argument_line(TextWriter& writer, Array<char>& out, u32 depth,
                                         const char* keyword,
                                         const ParameterArgument& argument) noexcept {
    if (Status opened = writer.begin_line(depth); !opened) {
        return opened;
    }
    if (Status written = writer.word(keyword); !written) {
        return written;
    }
    if (Status written = writer.word_u64(argument.id.value()); !written) {
        return written;
    }
    if (Status written = write_blob(writer, out, argument.wire, argument.value.span()); !written) {
        return written;
    }
    return writer.end_line();
}

[[nodiscard]] Status write_document_header(TextWriter& writer, const Document& document) noexcept {
    if (Status written = write_header_line(writer, "cydoc", kDocumentTextVersion); !written) {
        return written;
    }
    if (Status written = write_word_line(writer, "kind", asset_kind_name(document.kind));
        !written) {
        return written;
    }
    if (Status written = write_asset_line(writer, "id", document.id); !written) {
        return written;
    }
    if (Status written = write_header_line(writer, "schema", document.schema_version); !written) {
        return written;
    }
    return write_header_line(writer, "next", document.next_local_id());
}

/// A prefab variant's base: the asset, the mapping, the re-defaults and the overrides.
[[nodiscard]] Status write_variant_base(TextWriter& writer, Array<char>& out,
                                        const Document& document) noexcept {
    if (Status written = write_asset_line(writer, "base", document.base()); !written) {
        return written;
    }
    for (const InstanceMapping& entry : document.base_mapping()) {
        if (Status written = write_mapping_line(writer, 0, "basemap", entry); !written) {
            return written;
        }
    }
    for (const ParameterArgument& argument : document.base_arguments()) {
        if (Status written = write_argument_line(writer, out, 0, "basearg", argument); !written) {
            return written;
        }
    }
    for (const Override& item : document.base_overrides()) {
        if (Status written = write_override(writer, out, 0, "baseoverride", item); !written) {
            return written;
        }
    }
    return ok();
}

[[nodiscard]] Status write_parameters(TextWriter& writer, Array<char>& out,
                                      const Document& document) noexcept {
    for (const ExposedParameter& parameter : document.parameters()) {
        if (Status opened = writer.begin_line(0); !opened) {
            return opened;
        }
        if (Status written = writer.word("parameter"); !written) {
            return written;
        }
        if (Status written = writer.word_u64(parameter.id.value()); !written) {
            return written;
        }
        if (Status written = writer.word(serialize::wire_type_name(parameter.wire)); !written) {
            return written;
        }
        if (Status written = writer.word_quoted(document.text(parameter.name)); !written) {
            return written;
        }
        if (Status written = writer.word_quoted(document.text(parameter.documentation)); !written) {
            return written;
        }
        if (Status closed = writer.end_line(); !closed) {
            return closed;
        }

        if (!parameter.default_value().empty()) {
            if (Status opened = writer.begin_line(1); !opened) {
                return opened;
            }
            if (Status written = writer.word("default"); !written) {
                return written;
            }
            if (Status written =
                    write_blob(writer, out, parameter.wire, parameter.default_value().span());
                !written) {
                return written;
            }
            if (Status closed = writer.end_line(); !closed) {
                return closed;
            }
        }
        for (const ParameterBinding& binding : parameter.bindings()) {
            if (Status written = write_binding_line(writer, binding); !written) {
                return written;
            }
        }
    }
    return ok();
}

[[nodiscard]] Status write_entities(TextWriter& writer, const Document& document) noexcept {
    for (const DocumentEntity& entity : document.entities()) {
        if (Status opened = writer.begin_line(0); !opened) {
            return opened;
        }
        if (Status written = writer.word("entity"); !written) {
            return written;
        }
        if (Status written = writer.word_u64(entity.id.value()); !written) {
            return written;
        }
        if (Status written = writer.word_u64(entity.parent.value()); !written) {
            return written;
        }
        if (Status written = writer.word(motion_name(entity.motion)); !written) {
            return written;
        }
        if (Status written = writer.word(flatten_policy_name(entity.flatten)); !written) {
            return written;
        }
        if (Status written = writer.word_quoted(document.text(entity.name)); !written) {
            return written;
        }
        if (Status closed = writer.end_line(); !closed) {
            return closed;
        }
        for (const ComponentData& component : entity.components()) {
            if (Status written = writer.write_record(1, component.record); !written) {
                return written;
            }
        }
    }
    return ok();
}

[[nodiscard]] Status write_instance_header(TextWriter& writer, const Document& document,
                                           const Instance& instance,
                                           Array<char>& scratch) noexcept {
    if (Status opened = writer.begin_line(0); !opened) {
        return opened;
    }
    if (Status written = writer.word("instance"); !written) {
        return written;
    }
    if (Status written = writer.word_u64(instance.id.value()); !written) {
        return written;
    }
    if (Status written = writer.word_u64(instance.parent.value()); !written) {
        return written;
    }
    if (Status written = write_asset_id(writer, instance.source, scratch); !written) {
        return written;
    }
    if (Status written = writer.word(cook_mode_name(instance.cook_mode)); !written) {
        return written;
    }
    if (Status written = writer.word_quoted(document.text(instance.name)); !written) {
        return written;
    }
    return writer.end_line();
}

[[nodiscard]] Status write_instances(TextWriter& writer, Array<char>& out, const Document& document,
                                     Array<char>& scratch) noexcept {
    for (const Instance& instance : document.instances()) {
        if (Status written = write_instance_header(writer, document, instance, scratch); !written) {
            return written;
        }
        // The identity placement is the common case and writing it would put eighty hexadecimal
        // digits into every scene file for no information at all.
        if (instance.transform != cy::Transform::identity()) {
            if (Status opened = writer.begin_line(1); !opened) {
                return opened;
            }
            if (Status written = writer.word("transform"); !written) {
                return written;
            }
            if (Status written =
                    write_hex(writer, &instance.transform, sizeof(instance.transform), scratch);
                !written) {
                return written;
            }
            if (Status closed = writer.end_line(); !closed) {
                return closed;
            }
        }
        for (const InstanceMapping& entry : instance.mapping()) {
            if (Status written = write_mapping_line(writer, 1, "map", entry); !written) {
                return written;
            }
        }
        for (const ParameterArgument& argument : instance.arguments()) {
            if (Status written = write_argument_line(writer, out, 1, "arg", argument); !written) {
                return written;
            }
        }
        for (const Override& item : instance.overrides()) {
            if (Status written = write_override(writer, out, 1, "override", item); !written) {
                return written;
            }
        }
    }
    return ok();
}

}  // namespace

Status write_text(const Document& document, Array<char>& out,
                  serialize::TextOptions options) noexcept {
    out.clear();
    TextWriter writer(out, options);
    Array<char> scratch(out.allocator());

    if (Status written = write_document_header(writer, document); !written) {
        return written;
    }
    if (document.is_variant()) {
        if (Status written = write_variant_base(writer, out, document); !written) {
            return written;
        }
    }
    if (Status written = write_parameters(writer, out, document); !written) {
        return written;
    }
    if (Status written = write_entities(writer, document); !written) {
        return written;
    }
    return write_instances(writer, out, document, scratch);
}

namespace {

/// One pass over a document's text form.
///
/// A class rather than one long loop, and the reason is legibility rather than taste: the grammar
/// has thirteen keywords, and a single function that handled them all was — measured — the most
/// complex function in the engine by a factor of five. Each keyword is now a method that does one
/// thing, and `run()` is the dispatch and the three pieces of state a nested line needs to know
/// about: the instance a `map`/`arg`/`override` belongs to, the parameter a `default`/`bind`
/// belongs to, and the entity a `record` belongs to.
class DocumentTextReader {
public:
    DocumentTextReader(std::string_view text, Document& out) noexcept
        : scanner_(text), out_(&out), unquoted_(out.allocator()) {}

    [[nodiscard]] Status run() noexcept;

private:
    [[nodiscard]] Status dispatch(const TextLine& line, std::string_view keyword) noexcept;

    [[nodiscard]] static Status read_version(const TextLine& line) noexcept;
    [[nodiscard]] Status read_kind(const TextLine& line) noexcept;
    [[nodiscard]] Status read_asset(const TextLine& line, bool is_base) noexcept;
    [[nodiscard]] Status read_number(const TextLine& line, bool is_schema) noexcept;
    [[nodiscard]] Status read_mapping_line(const TextLine& line, bool is_base) noexcept;
    [[nodiscard]] Status read_argument_line(const TextLine& line, bool is_base) noexcept;
    [[nodiscard]] Status read_override_line(const TextLine& line, bool is_base) noexcept;
    [[nodiscard]] Status read_parameter_line(const TextLine& line) noexcept;
    [[nodiscard]] Status read_default_line(const TextLine& line) noexcept;
    [[nodiscard]] Status read_bind_line(const TextLine& line) noexcept;
    [[nodiscard]] Status read_entity_line(const TextLine& line) noexcept;
    [[nodiscard]] Status read_instance_line(const TextLine& line) noexcept;
    [[nodiscard]] Status read_transform_line(const TextLine& line) noexcept;
    [[nodiscard]] Status read_record_line(const TextLine& line) noexcept;

    TextScanner scanner_;
    Document* out_;
    Array<char> unquoted_;

    /// The instance a nested `map`, `arg` or `override` belongs to, and the parameter a nested
    /// `default` or `bind` belongs to. Pointers, because both are stable for as long as the block
    /// they head is being read: nothing adds another instance or parameter in between.
    Instance* instance_ = nullptr;
    ExposedParameter* parameter_ = nullptr;
    /// The entity a `record` line belongs to, held as an id rather than a pointer: entities are
    /// kept in a growable array, and adding one moves the others.
    LocalId entity_;
    /// The local-id counter the file declared. Restored last, because adding an entity advances the
    /// document's own counter past its id and the file's value is the authority.
    u32 declared_next_ = 1;
};

Status DocumentTextReader::read_version(const TextLine& line) noexcept {
    const Expected<u64, Error> version = line.word_u64(1);
    if (!version) {
        return make_unexpected(version.error());
    }
    if (version.value() > kDocumentTextVersion) {
        return fail(ErrorCode::Unsupported, "the document was written by a newer text format");
    }
    return ok();
}

Status DocumentTextReader::read_kind(const TextLine& line) noexcept {
    const Expected<AssetKind, Error> kind = asset_kind_from_name(line.word(1));
    if (!kind) {
        return make_unexpected(kind.error());
    }
    out_->kind = kind.value();
    return ok();
}

Status DocumentTextReader::read_asset(const TextLine& line, bool is_base) noexcept {
    const Expected<AssetId, Error> id = read_asset_id(line.word(1));
    if (!id) {
        return make_unexpected(id.error());
    }
    if (is_base) {
        out_->set_base(id.value());
    } else {
        out_->id = id.value();
    }
    return ok();
}

Status DocumentTextReader::read_number(const TextLine& line, bool is_schema) noexcept {
    const Expected<u64, Error> value = line.word_u64(1);
    if (!value) {
        return make_unexpected(value.error());
    }
    if (is_schema) {
        out_->schema_version = static_cast<u16>(value.value());
    } else {
        declared_next_ = static_cast<u32>(value.value());
    }
    return ok();
}

Status DocumentTextReader::read_mapping_line(const TextLine& line, bool is_base) noexcept {
    const Expected<u64, Error> source = line.word_u64(1);
    const Expected<u64, Error> local = line.word_u64(2);
    if (!source || !local) {
        return fail(ErrorCode::InvalidArgument, "a mapping line carries two numbers");
    }
    Array<InstanceMapping>& mapping =
        (is_base || instance_ == nullptr) ? out_->base_mapping() : instance_->mapping();
    return add_mapping(mapping, LocalId(static_cast<u32>(source.value())),
                       LocalId(static_cast<u32>(local.value())));
}

Status DocumentTextReader::read_argument_line(const TextLine& line, bool is_base) noexcept {
    const Expected<u64, Error> id = line.word_u64(1);
    if (!id) {
        return make_unexpected(id.error());
    }
    ParameterArgument argument;
    argument.id = ParameterId(static_cast<u32>(id.value()));
    argument.value = Array<u8>(out_->allocator());
    if (Status read = read_blob(line, 2, argument.wire, argument.value); !read) {
        return read;
    }
    Array<ParameterArgument>& target =
        (is_base || instance_ == nullptr) ? out_->base_arguments() : instance_->arguments();
    return target.push_back(std::move(argument));
}

Status DocumentTextReader::read_override_line(const TextLine& line, bool is_base) noexcept {
    if (is_base) {
        return read_override(scanner_, line, out_->base_overrides());
    }
    if (instance_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "an override line outside an instance");
    }
    return read_override(scanner_, line, instance_->overrides());
}

Status DocumentTextReader::read_parameter_line(const TextLine& line) noexcept {
    const Expected<u64, Error> id = line.word_u64(1);
    if (!id) {
        return make_unexpected(id.error());
    }
    const WireType wire = wire_from_name(line.word(2));
    if (wire == WireType::Count) {
        return fail(ErrorCode::Unsupported, "unknown wire type on a parameter");
    }
    const Expected<std::string_view, Error> name = line.word_unquoted(3, unquoted_);
    if (!name) {
        return make_unexpected(name.error());
    }
    const Expected<ExposedParameter*, Error> added =
        out_->add_parameter_with_id(ParameterId(static_cast<u32>(id.value())), name.value(), wire);
    if (!added) {
        return make_unexpected(added.error());
    }
    parameter_ = added.value();

    // The documentation is unquoted into a second buffer, because `unquoted_` still holds the name
    // the parameter was created from.
    Array<char> documentation(out_->allocator());
    const Expected<std::string_view, Error> text = line.word_unquoted(4, documentation);
    if (!text || text->empty()) {
        return ok();
    }
    const Expected<TextRef, Error> interned = out_->intern(text.value());
    if (!interned) {
        return make_unexpected(interned.error());
    }
    parameter_->documentation = interned.value();
    return ok();
}

Status DocumentTextReader::read_default_line(const TextLine& line) noexcept {
    if (parameter_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a default line outside a parameter");
    }
    WireType wire = WireType::Bytes;
    return read_blob(line, 1, wire, parameter_->default_value());
}

Status DocumentTextReader::read_bind_line(const TextLine& line) noexcept {
    if (parameter_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a bind line outside a parameter");
    }
    const Expected<u64, Error> entity = line.word_u64(1);
    const Expected<u64, Error> component = line.word_u64(2);
    const Expected<u64, Error> field = line.word_u64(3);
    if (!entity || !component || !field) {
        return fail(ErrorCode::InvalidArgument, "a bind line carries three numbers");
    }
    return parameter_->bindings().push_back(
        ParameterBinding{LocalId(static_cast<u32>(entity.value())),
                         reflect::TypeId(static_cast<u32>(component.value())),
                         reflect::FieldId(static_cast<u32>(field.value()))});
}

Status DocumentTextReader::read_entity_line(const TextLine& line) noexcept {
    const Expected<u64, Error> id = line.word_u64(1);
    const Expected<u64, Error> parent = line.word_u64(2);
    if (!id || !parent) {
        return fail(ErrorCode::InvalidArgument, "an entity line carries two numbers");
    }
    const Expected<MotionKind, Error> motion = motion_from_name(line.word(3));
    if (!motion) {
        return make_unexpected(motion.error());
    }
    const Expected<FlattenPolicy, Error> flatten = flatten_from_name(line.word(4));
    if (!flatten) {
        return make_unexpected(flatten.error());
    }
    const Expected<std::string_view, Error> name = line.word_unquoted(5, unquoted_);
    if (!name) {
        return make_unexpected(name.error());
    }
    const Expected<DocumentEntity*, Error> added =
        out_->add_entity_with_id(LocalId(static_cast<u32>(id.value())),
                                 LocalId(static_cast<u32>(parent.value())), name.value());
    if (!added) {
        return make_unexpected(added.error());
    }
    (*added)->motion = motion.value();
    (*added)->flatten = flatten.value();
    entity_ = (*added)->id;
    return ok();
}

Status DocumentTextReader::read_instance_line(const TextLine& line) noexcept {
    const Expected<u64, Error> id = line.word_u64(1);
    const Expected<u64, Error> parent = line.word_u64(2);
    if (!id || !parent) {
        return fail(ErrorCode::InvalidArgument, "an instance line carries two numbers");
    }
    const Expected<AssetId, Error> source = read_asset_id(line.word(3));
    if (!source) {
        return make_unexpected(source.error());
    }
    const Expected<CookMode, Error> mode = cook_mode_from_name(line.word(4));
    if (!mode) {
        return make_unexpected(mode.error());
    }
    const Expected<std::string_view, Error> name = line.word_unquoted(5, unquoted_);
    if (!name) {
        return make_unexpected(name.error());
    }
    const Expected<Instance*, Error> added =
        out_->add_instance_with_id(LocalId(static_cast<u32>(id.value())), source.value(),
                                   LocalId(static_cast<u32>(parent.value())), name.value());
    if (!added) {
        return make_unexpected(added.error());
    }
    instance_ = added.value();
    instance_->cook_mode = mode.value();
    return ok();
}

Status DocumentTextReader::read_transform_line(const TextLine& line) noexcept {
    if (instance_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a transform line outside an instance");
    }
    return read_hex(line.word(1), &instance_->transform, sizeof(instance_->transform));
}

Status DocumentTextReader::read_record_line(const TextLine& line) noexcept {
    DocumentEntity* entity = out_->find_entity(entity_);
    if (entity == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a record line before any entity");
    }
    const Expected<u64, Error> type = line.word_u64(1);
    if (!type) {
        return make_unexpected(type.error());
    }
    const Expected<ComponentData*, Error> component =
        entity->ensure(reflect::TypeId(static_cast<u32>(type.value())));
    if (!component) {
        return make_unexpected(component.error());
    }
    return serialize::read_record_text(scanner_, line, (*component)->record);
}

Status DocumentTextReader::dispatch(const TextLine& line, std::string_view keyword) noexcept {
    if (keyword == "cydoc") {
        return read_version(line);
    }
    if (keyword == "kind") {
        return read_kind(line);
    }
    if (keyword == "id" || keyword == "base") {
        return read_asset(line, keyword == "base");
    }
    if (keyword == "schema" || keyword == "next") {
        return read_number(line, keyword == "schema");
    }
    if (keyword == "basemap" || keyword == "map") {
        return read_mapping_line(line, keyword == "basemap");
    }
    if (keyword == "basearg" || keyword == "arg") {
        return read_argument_line(line, keyword == "basearg");
    }
    if (keyword == "baseoverride" || keyword == "override") {
        return read_override_line(line, keyword == "baseoverride");
    }
    if (keyword == "parameter") {
        return read_parameter_line(line);
    }
    if (keyword == "default") {
        return read_default_line(line);
    }
    if (keyword == "bind") {
        return read_bind_line(line);
    }
    if (keyword == "entity") {
        return read_entity_line(line);
    }
    if (keyword == "instance") {
        return read_instance_line(line);
    }
    if (keyword == "transform") {
        return read_transform_line(line);
    }
    if (keyword == "record") {
        return read_record_line(line);
    }
    return fail(ErrorCode::InvalidArgument, "unknown keyword in a document text form");
}

Status DocumentTextReader::run() noexcept {
    while (true) {
        const Expected<TextLine, Error> line = scanner_.next();
        if (!line) {
            break;
        }
        const std::string_view keyword = line->word(0);
        // A line at the outermost depth closes whatever block was open. `record` is the exception:
        // it is nested under an entity, and an entity is written at depth zero.
        if (line->depth() == 0 && keyword != "record") {
            instance_ = nullptr;
            parameter_ = nullptr;
        }
        if (Status read = dispatch(*line, keyword); !read) {
            return read;
        }
    }
    if (declared_next_ > out_->next_local_id()) {
        out_->set_next_local_id(declared_next_);
    }
    return ok();
}

}  // namespace

Status read_text(std::string_view text, Document& out) noexcept {
    if (!out.entities().empty() || !out.instances().empty() || !out.parameters().empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "read into a fresh document, not over a loaded one");
    }
    DocumentTextReader reader(text, out);
    return reader.run();
}

}  // namespace cy::scene::serialization
