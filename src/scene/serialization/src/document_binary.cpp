// The tagged binary form of an authoring document. Task 3.2.3.
//
// The same four things the text form carries, in the same order, addressed by the same identifiers
// — which is what makes text → binary → text reproduce the file. What differs is only the spelling:
// chunks and lengths instead of lines and indentation.
//
//   chunk 'DHDR'  the header: kind, schema version, id counter, asset id, variant base
//   chunk 'DBAS'  a variant's base mapping, re-defaults and overrides
//   chunk 'DPRM'  the exposed parameters, their defaults and their bindings
//   chunk 'DENT'  the entities and their components, as tagged records
//   chunk 'DINS'  the instances: mapping, arguments, overrides, transform, cook mode
//
// A chunk carries its length, so a reader that does not know a tag steps over it — which is how a
// later milestone adds, say, a cell-assignment chunk without every existing reader needing to know.
// The component data inside 'DENT' is written with the ordinary tagged record encoding, so a field
// this build has never heard of survives the round trip for exactly the reason it does everywhere
// else: a `ValueRecord` has no schema to check it against.

#include <cy/core/serialize/tagged.h>
#include <cy/scene/serialization/format.h>

#include <cstring>
#include <utility>

namespace cy::scene::serialization {
namespace {

using serialize::ByteReader;
using serialize::ByteWriter;
using serialize::TaggedChunk;
using serialize::TaggedReader;
using serialize::TaggedWriter;
using serialize::ValueRecord;
using serialize::WireType;

constexpr u32 kHeaderChunk = serialize::chunk_tag('D', 'H', 'D', 'R');
constexpr u32 kBaseChunk = serialize::chunk_tag('D', 'B', 'A', 'S');
constexpr u32 kParameterChunk = serialize::chunk_tag('D', 'P', 'R', 'M');
constexpr u32 kEntityChunk = serialize::chunk_tag('D', 'E', 'N', 'T');
constexpr u32 kInstanceChunk = serialize::chunk_tag('D', 'I', 'N', 'S');

[[nodiscard]] Status write_text_value(ByteWriter& writer, std::string_view value) noexcept {
    if (Status written = writer.write_u32(static_cast<u32>(value.size())); !written) {
        return written;
    }
    return writer.write_bytes(value.data(), value.size());
}

[[nodiscard]] Expected<std::string_view, Error> read_text_value(ByteReader& reader) noexcept {
    const Expected<u32, Error> length = reader.read_u32();
    if (!length) {
        return make_unexpected(length.error());
    }
    const Expected<Span<const u8>, Error> bytes = reader.read_bytes(length.value());
    if (!bytes) {
        return make_unexpected(bytes.error());
    }
    return std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

[[nodiscard]] Status write_blob(ByteWriter& writer, WireType wire, Span<const u8> bytes) noexcept {
    if (Status written = writer.write_u8(static_cast<u8>(wire)); !written) {
        return written;
    }
    if (Status written = writer.write_u32(static_cast<u32>(bytes.size())); !written) {
        return written;
    }
    return writer.write_bytes(bytes.data(), bytes.size());
}

[[nodiscard]] Status read_blob(ByteReader& reader, WireType& wire, Array<u8>& out) noexcept {
    const Expected<u8, Error> tag = reader.read_u8();
    if (!tag) {
        return make_unexpected(tag.error());
    }
    if (tag.value() >= static_cast<u8>(WireType::Count)) {
        return fail(ErrorCode::Unsupported, "unknown wire type in a document blob");
    }
    wire = static_cast<WireType>(tag.value());
    const Expected<u32, Error> length = reader.read_u32();
    if (!length) {
        return make_unexpected(length.error());
    }
    const Expected<Span<const u8>, Error> bytes = reader.read_bytes(length.value());
    if (!bytes) {
        return make_unexpected(bytes.error());
    }
    out.clear();
    return out.append(bytes.value());
}

[[nodiscard]] Status write_overrides(TaggedWriter& tagged, ByteWriter& writer,
                                     const OverrideList& list) noexcept {
    if (Status written = writer.write_u32(static_cast<u32>(list.size())); !written) {
        return written;
    }
    for (const Override& item : list) {
        if (Status written = writer.write_u8(static_cast<u8>(item.op())); !written) {
            return written;
        }
        if (Status written = writer.write_u32(item.target().entity.value()); !written) {
            return written;
        }
        if (Status written = writer.write_u32(item.target().component.value()); !written) {
            return written;
        }
        if (Status written = writer.write_u32(item.target().field.value()); !written) {
            return written;
        }
        if (Status written = writer.write_u32(item.parent().value()); !written) {
            return written;
        }
        if (Status written = writer.write_u16(item.schema_version()); !written) {
            return written;
        }
        if (Status written = writer.write_u8(static_cast<u8>(item.conflict())); !written) {
            return written;
        }
        const bool has_payload = item.payload().type().valid();
        if (Status written = writer.write_u8(has_payload ? 1U : 0U); !written) {
            return written;
        }
        if (has_payload) {
            if (Status written = tagged.write_record(item.payload()); !written) {
                return written;
            }
        }
    }
    return ok();
}

[[nodiscard]] Status read_overrides(ByteReader& reader, OverrideList& out) noexcept {
    const Expected<u32, Error> count = reader.read_u32();
    if (!count) {
        return make_unexpected(count.error());
    }
    for (u32 index = 0; index < count.value(); ++index) {
        Override item(out.allocator());
        const Expected<u8, Error> op = reader.read_u8();
        const Expected<u32, Error> entity = reader.read_u32();
        const Expected<u32, Error> component = reader.read_u32();
        const Expected<u32, Error> field = reader.read_u32();
        const Expected<u32, Error> parent = reader.read_u32();
        const Expected<u16, Error> schema = reader.read_u16();
        const Expected<u8, Error> conflict = reader.read_u8();
        const Expected<u8, Error> has_payload = reader.read_u8();
        if (!op || !entity || !component || !field || !parent || !schema || !conflict ||
            !has_payload) {
            return fail(ErrorCode::OutOfRange, "an override record is truncated");
        }
        if (op.value() > static_cast<u8>(OverrideOp::ReparentEntity) ||
            conflict.value() > static_cast<u8>(ConflictKind::MissingParent)) {
            return fail(ErrorCode::Unsupported, "an override this build does not define");
        }
        item.set_op(static_cast<OverrideOp>(op.value()));
        item.set_target(OverrideTarget{LocalId(entity.value()), reflect::TypeId(component.value()),
                                       reflect::FieldId(field.value())});
        item.set_parent(LocalId(parent.value()));
        item.set_schema_version(schema.value());
        item.set_conflict(static_cast<ConflictKind>(conflict.value()));
        if (has_payload.value() != 0) {
            if (Status read = serialize::read_record(reader, item.payload()); !read) {
                return read;
            }
        }
        if (Status added = out.add(std::move(item)); !added) {
            return added;
        }
    }
    return ok();
}

[[nodiscard]] Status write_mapping(ByteWriter& writer,
                                   Span<const InstanceMapping> mapping) noexcept {
    if (Status written = writer.write_u32(static_cast<u32>(mapping.size())); !written) {
        return written;
    }
    for (const InstanceMapping& entry : mapping) {
        if (Status written = writer.write_u32(entry.source.value()); !written) {
            return written;
        }
        if (Status written = writer.write_u32(entry.local.value()); !written) {
            return written;
        }
    }
    return ok();
}

[[nodiscard]] Status read_mapping(ByteReader& reader, Array<InstanceMapping>& out) noexcept {
    const Expected<u32, Error> count = reader.read_u32();
    if (!count) {
        return make_unexpected(count.error());
    }
    for (u32 index = 0; index < count.value(); ++index) {
        const Expected<u32, Error> source = reader.read_u32();
        const Expected<u32, Error> local = reader.read_u32();
        if (!source || !local) {
            return fail(ErrorCode::OutOfRange, "a mapping entry is truncated");
        }
        if (Status added = add_mapping(out, LocalId(source.value()), LocalId(local.value()));
            !added) {
            return added;
        }
    }
    return ok();
}

[[nodiscard]] Status write_arguments(ByteWriter& writer,
                                     Span<const ParameterArgument> arguments) noexcept {
    if (Status written = writer.write_u32(static_cast<u32>(arguments.size())); !written) {
        return written;
    }
    for (const ParameterArgument& argument : arguments) {
        if (Status written = writer.write_u32(argument.id.value()); !written) {
            return written;
        }
        if (Status written = write_blob(writer, argument.wire, argument.value.span()); !written) {
            return written;
        }
    }
    return ok();
}

[[nodiscard]] Status read_arguments(ByteReader& reader, Array<ParameterArgument>& out,
                                    Allocator& allocator) noexcept {
    const Expected<u32, Error> count = reader.read_u32();
    if (!count) {
        return make_unexpected(count.error());
    }
    for (u32 index = 0; index < count.value(); ++index) {
        const Expected<u32, Error> id = reader.read_u32();
        if (!id) {
            return make_unexpected(id.error());
        }
        ParameterArgument argument;
        argument.id = ParameterId(id.value());
        argument.value = Array<u8>(allocator);
        if (Status read = read_blob(reader, argument.wire, argument.value); !read) {
            return read;
        }
        if (Status added = out.push_back(std::move(argument)); !added) {
            return added;
        }
    }
    return ok();
}

}  // namespace

namespace {

[[nodiscard]] Status write_header_chunk(TaggedWriter& tagged, ByteWriter& writer,
                                        const Document& document) noexcept {
    if (Status opened = tagged.begin_chunk(kHeaderChunk); !opened) {
        return opened;
    }
    if (Status written = writer.write_u8(static_cast<u8>(document.kind)); !written) {
        return written;
    }
    if (Status written = writer.write_u16(document.schema_version); !written) {
        return written;
    }
    if (Status written = writer.write_u32(document.next_local_id()); !written) {
        return written;
    }
    if (Status written = writer.write_u64(document.id.high()); !written) {
        return written;
    }
    if (Status written = writer.write_u64(document.id.low()); !written) {
        return written;
    }
    if (Status written = writer.write_u64(document.base().high()); !written) {
        return written;
    }
    if (Status written = writer.write_u64(document.base().low()); !written) {
        return written;
    }
    return tagged.end_chunk();
}

[[nodiscard]] Status write_base_chunk(TaggedWriter& tagged, ByteWriter& writer,
                                      const Document& document) noexcept {
    if (Status opened = tagged.begin_chunk(kBaseChunk); !opened) {
        return opened;
    }
    if (Status written = write_mapping(writer, document.base_mapping().span()); !written) {
        return written;
    }
    if (Status written = write_arguments(writer, document.base_arguments().span()); !written) {
        return written;
    }
    if (Status written = write_overrides(tagged, writer, document.base_overrides()); !written) {
        return written;
    }
    return tagged.end_chunk();
}

[[nodiscard]] Status write_parameter(ByteWriter& writer, const Document& document,
                                     const ExposedParameter& parameter) noexcept {
    if (Status written = writer.write_u32(parameter.id.value()); !written) {
        return written;
    }
    if (Status written = write_text_value(writer, document.text(parameter.name)); !written) {
        return written;
    }
    if (Status written = write_text_value(writer, document.text(parameter.documentation));
        !written) {
        return written;
    }
    if (Status written = write_blob(writer, parameter.wire, parameter.default_value().span());
        !written) {
        return written;
    }
    if (Status written = writer.write_u32(static_cast<u32>(parameter.bindings().size()));
        !written) {
        return written;
    }
    for (const ParameterBinding& binding : parameter.bindings()) {
        if (Status written = writer.write_u32(binding.entity.value()); !written) {
            return written;
        }
        if (Status written = writer.write_u32(binding.component.value()); !written) {
            return written;
        }
        if (Status written = writer.write_u32(binding.field.value()); !written) {
            return written;
        }
    }
    return ok();
}

[[nodiscard]] Status write_parameter_chunk(TaggedWriter& tagged, ByteWriter& writer,
                                           const Document& document) noexcept {
    if (Status opened = tagged.begin_chunk(kParameterChunk); !opened) {
        return opened;
    }
    if (Status written = writer.write_u32(static_cast<u32>(document.parameters().size()));
        !written) {
        return written;
    }
    for (const ExposedParameter& parameter : document.parameters()) {
        if (Status written = write_parameter(writer, document, parameter); !written) {
            return written;
        }
    }
    return tagged.end_chunk();
}

[[nodiscard]] Status write_entity(TaggedWriter& tagged, ByteWriter& writer,
                                  const Document& document, const DocumentEntity& entity) noexcept {
    if (Status written = writer.write_u32(entity.id.value()); !written) {
        return written;
    }
    if (Status written = writer.write_u32(entity.parent.value()); !written) {
        return written;
    }
    if (Status written = writer.write_u8(static_cast<u8>(entity.motion)); !written) {
        return written;
    }
    if (Status written = writer.write_u8(static_cast<u8>(entity.flatten)); !written) {
        return written;
    }
    if (Status written = write_text_value(writer, document.text(entity.name)); !written) {
        return written;
    }
    if (Status written = writer.write_u32(static_cast<u32>(entity.components().size())); !written) {
        return written;
    }
    for (const ComponentData& component : entity.components()) {
        if (Status written = tagged.write_record(component.record); !written) {
            return written;
        }
    }
    return ok();
}

[[nodiscard]] Status write_entity_chunk(TaggedWriter& tagged, ByteWriter& writer,
                                        const Document& document) noexcept {
    if (Status opened = tagged.begin_chunk(kEntityChunk); !opened) {
        return opened;
    }
    if (Status written = writer.write_u32(static_cast<u32>(document.entities().size())); !written) {
        return written;
    }
    for (const DocumentEntity& entity : document.entities()) {
        if (Status written = write_entity(tagged, writer, document, entity); !written) {
            return written;
        }
    }
    return tagged.end_chunk();
}

[[nodiscard]] Status write_instance(TaggedWriter& tagged, ByteWriter& writer,
                                    const Document& document, const Instance& instance) noexcept {
    if (Status written = writer.write_u32(instance.id.value()); !written) {
        return written;
    }
    if (Status written = writer.write_u32(instance.parent.value()); !written) {
        return written;
    }
    if (Status written = writer.write_u64(instance.source.high()); !written) {
        return written;
    }
    if (Status written = writer.write_u64(instance.source.low()); !written) {
        return written;
    }
    if (Status written = writer.write_u8(static_cast<u8>(instance.cook_mode)); !written) {
        return written;
    }
    if (Status written = write_text_value(writer, document.text(instance.name)); !written) {
        return written;
    }
    if (Status written = writer.write_bytes(&instance.transform, sizeof(instance.transform));
        !written) {
        return written;
    }
    if (Status written = write_mapping(writer, instance.mapping().span()); !written) {
        return written;
    }
    if (Status written = write_arguments(writer, instance.arguments().span()); !written) {
        return written;
    }
    return write_overrides(tagged, writer, instance.overrides());
}

[[nodiscard]] Status write_instance_chunk(TaggedWriter& tagged, ByteWriter& writer,
                                          const Document& document) noexcept {
    if (Status opened = tagged.begin_chunk(kInstanceChunk); !opened) {
        return opened;
    }
    if (Status written = writer.write_u32(static_cast<u32>(document.instances().size()));
        !written) {
        return written;
    }
    for (const Instance& instance : document.instances()) {
        if (Status written = write_instance(tagged, writer, document, instance); !written) {
            return written;
        }
    }
    return tagged.end_chunk();
}

}  // namespace

Status write_binary(const Document& document, Array<u8>& out) noexcept {
    out.clear();
    TaggedWriter tagged(out);
    // Two writers over one buffer: the tagged one frames the chunks, and the raw one writes the
    // fields inside them. They share the array rather than any state, which is why the chunk
    // lengths the tagged writer patches still come out right.
    ByteWriter writer(out);
    if (Status begun = tagged.begin_stream(); !begun) {
        return begun;
    }
    if (Status written = write_header_chunk(tagged, writer, document); !written) {
        return written;
    }
    if (document.is_variant()) {
        if (Status written = write_base_chunk(tagged, writer, document); !written) {
            return written;
        }
    }
    if (Status written = write_parameter_chunk(tagged, writer, document); !written) {
        return written;
    }
    if (Status written = write_entity_chunk(tagged, writer, document); !written) {
        return written;
    }
    if (Status written = write_instance_chunk(tagged, writer, document); !written) {
        return written;
    }
    return tagged.end_stream();
}

namespace {

[[nodiscard]] Status read_header_chunk(ByteReader& reader, Document& out) noexcept {
    const Expected<u8, Error> kind = reader.read_u8();
    const Expected<u16, Error> schema = reader.read_u16();
    const Expected<u32, Error> next = reader.read_u32();
    const Expected<u64, Error> id_high = reader.read_u64();
    const Expected<u64, Error> id_low = reader.read_u64();
    const Expected<u64, Error> base_high = reader.read_u64();
    const Expected<u64, Error> base_low = reader.read_u64();
    if (!kind || !schema || !next || !id_high || !id_low || !base_high || !base_low) {
        return fail(ErrorCode::OutOfRange, "the document header is truncated");
    }
    if (kind.value() > static_cast<u8>(AssetKind::World)) {
        return fail(ErrorCode::Unsupported, "an asset kind this build does not define");
    }
    out.kind = static_cast<AssetKind>(kind.value());
    out.schema_version = schema.value();
    out.id = AssetId(id_high.value(), id_low.value());
    out.set_base(AssetId(base_high.value(), base_low.value()));
    out.set_next_local_id(next.value());
    return ok();
}

[[nodiscard]] Status read_parameter_bindings(ByteReader& reader,
                                             ExposedParameter& parameter) noexcept {
    const Expected<u32, Error> count = reader.read_u32();
    if (!count) {
        return make_unexpected(count.error());
    }
    for (u32 index = 0; index < count.value(); ++index) {
        const Expected<u32, Error> entity = reader.read_u32();
        const Expected<u32, Error> component = reader.read_u32();
        const Expected<u32, Error> field = reader.read_u32();
        if (!entity || !component || !field) {
            return fail(ErrorCode::OutOfRange, "a parameter binding is truncated");
        }
        if (Status added = parameter.bindings().push_back(
                ParameterBinding{LocalId(entity.value()), reflect::TypeId(component.value()),
                                 reflect::FieldId(field.value())});
            !added) {
            return added;
        }
    }
    return ok();
}

[[nodiscard]] Status read_parameter(ByteReader& reader, Document& out) noexcept {
    const Expected<u32, Error> id = reader.read_u32();
    if (!id) {
        return make_unexpected(id.error());
    }
    const Expected<std::string_view, Error> name = read_text_value(reader);
    if (!name) {
        return make_unexpected(name.error());
    }
    const Expected<std::string_view, Error> documentation = read_text_value(reader);
    if (!documentation) {
        return make_unexpected(documentation.error());
    }
    // The name is copied into the document's text pool here, because the view above addresses the
    // caller's bytes and not the document's.
    const Expected<ExposedParameter*, Error> parameter =
        out.add_parameter_with_id(ParameterId(id.value()), name.value(), WireType::Bytes);
    if (!parameter) {
        return make_unexpected(parameter.error());
    }
    if (!documentation->empty()) {
        const Expected<TextRef, Error> interned = out.intern(documentation.value());
        if (!interned) {
            return make_unexpected(interned.error());
        }
        (*parameter)->documentation = interned.value();
    }
    if (Status read = read_blob(reader, (*parameter)->wire, (*parameter)->default_value()); !read) {
        return read;
    }
    return read_parameter_bindings(reader, **parameter);
}

[[nodiscard]] Status read_parameter_chunk(ByteReader& reader, Document& out) noexcept {
    const Expected<u32, Error> count = reader.read_u32();
    if (!count) {
        return make_unexpected(count.error());
    }
    for (u32 index = 0; index < count.value(); ++index) {
        if (Status read = read_parameter(reader, out); !read) {
            return read;
        }
    }
    return ok();
}

[[nodiscard]] Status read_entity_chunk(ByteReader& reader, Document& out) noexcept {
    const Expected<u32, Error> count = reader.read_u32();
    if (!count) {
        return make_unexpected(count.error());
    }
    for (u32 index = 0; index < count.value(); ++index) {
        const Expected<u32, Error> id = reader.read_u32();
        const Expected<u32, Error> parent = reader.read_u32();
        const Expected<u8, Error> motion = reader.read_u8();
        const Expected<u8, Error> flatten = reader.read_u8();
        if (!id || !parent || !motion || !flatten) {
            return fail(ErrorCode::OutOfRange, "an entity record is truncated");
        }
        if (motion.value() > static_cast<u8>(MotionKind::Dynamic) ||
            flatten.value() > static_cast<u8>(FlattenPolicy::Flatten)) {
            return fail(ErrorCode::Unsupported, "an entity flag this build does not define");
        }
        const Expected<std::string_view, Error> name = read_text_value(reader);
        if (!name) {
            return make_unexpected(name.error());
        }
        const Expected<DocumentEntity*, Error> entity =
            out.add_entity_with_id(LocalId(id.value()), LocalId(parent.value()), name.value());
        if (!entity) {
            return make_unexpected(entity.error());
        }
        (*entity)->motion = static_cast<MotionKind>(motion.value());
        (*entity)->flatten = static_cast<FlattenPolicy>(flatten.value());

        const Expected<u32, Error> components = reader.read_u32();
        if (!components) {
            return make_unexpected(components.error());
        }
        for (u32 component = 0; component < components.value(); ++component) {
            ValueRecord record(out.allocator());
            if (Status read = serialize::read_record(reader, record); !read) {
                return read;
            }
            const Expected<ComponentData*, Error> slot = (*entity)->ensure(record.type());
            if (!slot) {
                return make_unexpected(slot.error());
            }
            (*slot)->record = std::move(record);
        }
    }
    return ok();
}

[[nodiscard]] Status read_instance_chunk(ByteReader& reader, Document& out) noexcept {
    const Expected<u32, Error> count = reader.read_u32();
    if (!count) {
        return make_unexpected(count.error());
    }
    for (u32 index = 0; index < count.value(); ++index) {
        const Expected<u32, Error> id = reader.read_u32();
        const Expected<u32, Error> parent = reader.read_u32();
        const Expected<u64, Error> source_high = reader.read_u64();
        const Expected<u64, Error> source_low = reader.read_u64();
        const Expected<u8, Error> mode = reader.read_u8();
        if (!id || !parent || !source_high || !source_low || !mode) {
            return fail(ErrorCode::OutOfRange, "an instance record is truncated");
        }
        if (mode.value() > static_cast<u8>(CookMode::Packed)) {
            return fail(ErrorCode::Unsupported, "a cook mode this build does not define");
        }
        const Expected<std::string_view, Error> name = read_text_value(reader);
        if (!name) {
            return make_unexpected(name.error());
        }
        const Expected<Instance*, Error> instance = out.add_instance_with_id(
            LocalId(id.value()), AssetId(source_high.value(), source_low.value()),
            LocalId(parent.value()), name.value());
        if (!instance) {
            return make_unexpected(instance.error());
        }
        (*instance)->cook_mode = static_cast<CookMode>(mode.value());

        const Expected<Span<const u8>, Error> transform =
            reader.read_bytes(sizeof((*instance)->transform));
        if (!transform) {
            return make_unexpected(transform.error());
        }
        std::memcpy(&(*instance)->transform, transform->data(), transform->size());

        if (Status read = read_mapping(reader, (*instance)->mapping()); !read) {
            return read;
        }
        if (Status read = read_arguments(reader, (*instance)->arguments(), out.allocator());
            !read) {
            return read;
        }
        if (Status read = read_overrides(reader, (*instance)->overrides()); !read) {
            return read;
        }
    }
    return ok();
}

[[nodiscard]] Status read_base_chunk(ByteReader& reader, Document& out) noexcept {
    if (Status read = read_mapping(reader, out.base_mapping()); !read) {
        return read;
    }
    if (Status read = read_arguments(reader, out.base_arguments(), out.allocator()); !read) {
        return read;
    }
    return read_overrides(reader, out.base_overrides());
}

}  // namespace

Status read_binary(Span<const u8> bytes, Document& out) noexcept {
    if (!out.entities().empty() || !out.instances().empty() || !out.parameters().empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "read into a fresh document, not over a loaded one");
    }

    TaggedReader tagged(bytes.data(), bytes.size());
    if (Status header = tagged.read_header(); !header) {
        return header;
    }
    u32 declared_next = 1;

    while (true) {
        const Expected<TaggedChunk, Error> chunk = tagged.next_chunk();
        if (!chunk) {
            if (chunk.error().code == ErrorCode::NotFound) {
                break;
            }
            return make_unexpected(chunk.error());
        }
        ByteReader reader(chunk->payload.data(), chunk->payload.size());
        Status read = ok();
        switch (chunk->tag) {
            case kHeaderChunk:
                read = read_header_chunk(reader, out);
                declared_next = out.next_local_id();
                break;
            case kBaseChunk:
                read = read_base_chunk(reader, out);
                break;
            case kParameterChunk:
                read = read_parameter_chunk(reader, out);
                break;
            case kEntityChunk:
                read = read_entity_chunk(reader, out);
                break;
            case kInstanceChunk:
                read = read_instance_chunk(reader, out);
                break;
            default:
                // A chunk written by a later build. Stepped over, which is what the length is for.
                break;
        }
        if (!read) {
            return read;
        }
    }

    // Restored last, for the same reason the text form restores it last: adding an entity advances
    // the counter past its id, and the file's own value is the authority.
    if (declared_next > out.next_local_id()) {
        out.set_next_local_id(declared_next);
    }
    return ok();
}

}  // namespace cy::scene::serialization
