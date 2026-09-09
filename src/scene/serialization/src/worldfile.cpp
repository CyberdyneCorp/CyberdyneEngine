// `.cyworld`, read and written by the engine. The header carries the argument; this file carries
// the grammar and the arithmetic.

#include <cy/core/serialize/text.h>
#include <cy/scene/serialization/worldfile.h>

#include <cstdlib>
#include <cstring>

namespace cy::scene::serialization {
namespace {

using serialize::TextLine;
using serialize::TextScanner;
using serialize::TextWriter;

// --- FNV-1a-128, exactly as `cy_editor_core::ids` computes it ------------------------------------
//
// The prime is 2^88 + 0x13b, so the multiply is a shift and a small multiply rather than a general
// 128x128. Written out because the shift form is the one that can be read and checked against the
// constant, and because the general form would need a 128-bit type this engine will not assume.

constexpr u64 kOffsetLow = 0x62b8'2175'6295'c58dULL;
constexpr u64 kOffsetHigh = 0x6c62'272e'07bb'0142ULL;
constexpr u64 kPrimeTail = 0x13bULL;

/// `value *= 2^88 + 0x13b`, modulo 2^128.
void multiply_by_prime(EditorId& value) noexcept {
    // The 2^88 term: everything below bit 88 of the product comes from `low`, and `low << 88`
    // keeps only what `low << 24` contributes to the high half. `high << 88` is entirely above
    // bit 128 and vanishes.
    const u64 shifted_high = value.low << 24U;

    // The 0x13b term, as a 64x64 -> 128 multiply built from two 32-bit halves.
    const u64 lower = (value.low & 0xFFFF'FFFFULL) * kPrimeTail;
    const u64 upper = (value.low >> 32U) * kPrimeTail;
    const u64 middle = upper + (lower >> 32U);
    const u64 product_low = (lower & 0xFFFF'FFFFULL) | (middle << 32U);
    const u64 product_high = (value.high * kPrimeTail) + (middle >> 32U);

    value.low = product_low;
    value.high = product_high + shifted_high;
}

[[nodiscard]] EditorId fnv1a_128(Span<const u8> bytes) noexcept {
    EditorId hash{kOffsetLow, kOffsetHigh};
    for (const u8 byte : bytes) {
        hash.low ^= static_cast<u64>(byte);
        multiply_by_prime(hash);
    }
    return hash;
}

/// The hex digit a nibble spells, in the lower case the editor writes.
[[nodiscard]] char hex_digit(u8 nibble) noexcept {
    return static_cast<char>((nibble < 10U) ? ('0' + nibble) : ('a' + (nibble - 10U)));
}

[[nodiscard]] bool hex_value(char character, u8& out) noexcept {
    if (character >= '0' && character <= '9') {
        out = static_cast<u8>(character - '0');
        return true;
    }
    if (character >= 'a' && character <= 'f') {
        out = static_cast<u8>((character - 'a') + 10);
        return true;
    }
    if (character >= 'A' && character <= 'F') {
        out = static_cast<u8>((character - 'A') + 10);
        return true;
    }
    return false;
}

/// How many lanes a kind writes. Zero for everything that is not a float vector.
[[nodiscard]] u32 lane_count(WorldValueKind kind) noexcept {
    switch (kind) {
        case WorldValueKind::Vec2:
            return 2;
        case WorldValueKind::Vec3:
            return 3;
        case WorldValueKind::Vec4:
        case WorldValueKind::Quat:
            return 4;
        default:
            return 0;
    }
}

[[nodiscard]] Expected<f32, Error> word_f32(const TextLine& line, usize index) noexcept {
    const std::string_view word = line.word(index);
    if (word.empty()) {
        return fail(ErrorCode::InvalidArgument, "a world field is missing a number");
    }
    // `std::from_chars` for floats is what `read_value_text` uses; a null-terminated copy keeps
    // this independent of whether the word is the last on the line.
    char buffer[64] = {};
    if (word.size() >= sizeof(buffer)) {
        return fail(ErrorCode::InvalidArgument, "a world field's number is implausibly long");
    }
    std::memcpy(buffer, word.data(), word.size());
    char* end = nullptr;
    const double parsed = strtod(buffer, &end);
    if (end == buffer) {
        return fail(ErrorCode::InvalidArgument, "a world field holds text where a number belongs");
    }
    return static_cast<f32>(parsed);
}

[[nodiscard]] Expected<f64, Error> word_f64(const TextLine& line, usize index) noexcept {
    const Expected<f32, Error> narrow = word_f32(line, index);
    if (!narrow) {
        return make_unexpected(narrow.error());
    }
    char buffer[64] = {};
    const std::string_view word = line.word(index);
    std::memcpy(buffer, word.data(), word.size());
    return strtod(buffer, nullptr);
}

/// A quoted word, interned into the world's blob pool.
[[nodiscard]] Status read_text_value(const TextLine& line, usize first, World& world,
                                     WorldValue& out) noexcept {
    Array<char> unescaped(world.allocator());
    const Expected<std::string_view, Error> text = line.word_unquoted(first, unescaped);
    if (!text) {
        return make_unexpected(text.error());
    }
    return world.intern_blob(
        Span<const u8>(reinterpret_cast<const u8*>(text->data()), text->size()), out);
}

/// A hex word, or `-` for nothing. The inverse of what `write_value` writes for `Bytes`.
[[nodiscard]] Status read_bytes_value(const TextLine& line, usize first, World& world,
                                      WorldValue& out) noexcept {
    const std::string_view word = line.word(first);
    if (word == "-") {
        return ok();
    }
    if ((word.size() % 2U) != 0U) {
        return fail(ErrorCode::InvalidArgument,
                    "a world byte field has an odd number of hex "
                    "digits");
    }
    Array<u8> bytes(world.allocator());
    for (usize index = 0; index + 1 < word.size(); index += 2) {
        u8 high = 0;
        u8 low = 0;
        if (!hex_value(word[index], high) || !hex_value(word[index + 1], low)) {
            return fail(ErrorCode::InvalidArgument, "a world byte field holds a non-hex character");
        }
        if (Status pushed = bytes.push_back(static_cast<u8>((high << 4U) | low)); !pushed) {
            return pushed;
        }
    }
    return world.intern_blob(bytes.span(), out);
}

/// Read one value of a declared kind, starting at `first`.
[[nodiscard]] Status read_value(const TextLine& line, usize first, WorldValueKind kind,
                                World& world, WorldValue& out) noexcept {
    out = WorldValue{};
    out.kind = kind;
    const u32 lanes = lane_count(kind);
    if (lanes != 0) {
        for (u32 lane = 0; lane < lanes; ++lane) {
            const Expected<f32, Error> value = word_f32(line, first + lane);
            if (!value) {
                return make_unexpected(value.error());
            }
            out.lanes[lane] = *value;
        }
        return ok();
    }
    switch (kind) {
        case WorldValueKind::Vec2:
        case WorldValueKind::Vec3:
        case WorldValueKind::Vec4:
        case WorldValueKind::Quat:
            break;  // handled above, where the lane count is what decides
        case WorldValueKind::Nil:
            return ok();
        case WorldValueKind::Bool:
            out.integer = (line.word(first) == "true") ? 1 : 0;
            return ok();
        case WorldValueKind::Int:
        case WorldValueKind::Entity: {
            const Expected<i64, Error> value = line.word_i64(first);
            if (!value) {
                return make_unexpected(value.error());
            }
            out.integer = *value;
            return ok();
        }
        case WorldValueKind::Float: {
            const Expected<f32, Error> value = word_f32(line, first);
            if (!value) {
                return make_unexpected(value.error());
            }
            out.lanes[0] = *value;
            return ok();
        }
        case WorldValueKind::Double: {
            const Expected<f64, Error> value = word_f64(line, first);
            if (!value) {
                return make_unexpected(value.error());
            }
            out.real = *value;
            return ok();
        }
        case WorldValueKind::Text:
            return read_text_value(line, first, world, out);
        case WorldValueKind::Bytes:
            return read_bytes_value(line, first, world, out);
    }
    return fail(ErrorCode::InvalidArgument, "a world field has a kind this build has not");
}

/// Append one value's words to a line the writer has open.
[[nodiscard]] Status write_value(TextWriter& writer, const World& world,
                                 const WorldValue& value) noexcept {
    char buffer[serialize::kFloatTextCapacity] = {};
    const u32 lanes = lane_count(value.kind);
    if (lanes != 0) {
        for (u32 lane = 0; lane < lanes; ++lane) {
            const Expected<usize, Error> written =
                serialize::format_f32(value.lanes[lane], buffer, sizeof(buffer));
            if (!written) {
                return make_unexpected(written.error());
            }
            if (Status word = writer.word(std::string_view(buffer, *written)); !word) {
                return word;
            }
        }
        return ok();
    }
    switch (value.kind) {
        case WorldValueKind::Nil:
            return writer.word("nil");
        case WorldValueKind::Bool:
            return writer.word((value.integer != 0) ? "true" : "false");
        case WorldValueKind::Int:
            return writer.word_i64(value.integer);
        case WorldValueKind::Entity:
            return writer.word_u64(static_cast<u64>(value.integer));
        case WorldValueKind::Float: {
            const Expected<usize, Error> written =
                serialize::format_f32(value.lanes[0], buffer, sizeof(buffer));
            if (!written) {
                return make_unexpected(written.error());
            }
            return writer.word(std::string_view(buffer, *written));
        }
        case WorldValueKind::Double: {
            const Expected<usize, Error> written =
                serialize::format_f64(value.real, buffer, sizeof(buffer));
            if (!written) {
                return make_unexpected(written.error());
            }
            return writer.word(std::string_view(buffer, *written));
        }
        case WorldValueKind::Text: {
            const Span<const u8> bytes = world.blob(value);
            return writer.word_quoted(
                std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        }
        case WorldValueKind::Bytes: {
            const Span<const u8> bytes = world.blob(value);
            if (bytes.empty()) {
                return writer.word("-");
            }
            Array<char> hex(world.allocator());
            for (const u8 byte : bytes) {
                if (Status pushed = hex.push_back(hex_digit(static_cast<u8>(byte >> 4U)));
                    !pushed) {
                    return pushed;
                }
                if (Status pushed = hex.push_back(hex_digit(static_cast<u8>(byte & 0x0FU)));
                    !pushed) {
                    return pushed;
                }
            }
            return writer.word(std::string_view(hex.data(), hex.size()));
        }
        default:
            break;
    }
    return fail(ErrorCode::InvalidArgument, "a world value has a kind this build has not");
}

// --- the schema half ------------------------------------------------------------------------

[[nodiscard]] Status read_type_line(const TextLine& line, World& world) noexcept {
    const Expected<u64, Error> file_type = line.word_u64(1);
    if (!file_type) {
        return make_unexpected(file_type.error());
    }
    const std::string_view discipline = line.word(2);
    if (discipline != "authoring" && discipline != "runtime") {
        return fail(ErrorCode::InvalidArgument,
                    "a world type is neither 'authoring' nor 'runtime'");
    }
    Array<char> unescaped(world.allocator());
    const Expected<std::string_view, Error> name = line.word_unquoted(3, unescaped);
    if (!name) {
        return make_unexpected(name.error());
    }
    const Expected<WorldText, Error> interned = world.intern(*name);
    if (!interned) {
        return make_unexpected(interned.error());
    }
    WorldTypeDecl declaration(world.allocator());
    declaration.file_type = *file_type;
    declaration.authoring_only = discipline == "authoring";
    declaration.name = *interned;
    return world.types().push_back(std::move(declaration));
}

[[nodiscard]] Status read_field_line(const TextLine& line, World& world) noexcept {
    if (world.types().empty()) {
        return fail(ErrorCode::InvalidArgument, "a world field precedes every type");
    }
    const Expected<u64, Error> file_field = line.word_u64(1);
    if (!file_field) {
        return make_unexpected(file_field.error());
    }
    const Expected<WorldValueKind, Error> kind = world_value_kind_of(line.word(2));
    if (!kind) {
        return make_unexpected(kind.error());
    }
    Array<char> unescaped(world.allocator());
    const Expected<std::string_view, Error> name = line.word_unquoted(3, unescaped);
    if (!name) {
        return make_unexpected(name.error());
    }
    const Expected<WorldText, Error> interned_name = world.intern(*name);
    if (!interned_name) {
        return make_unexpected(interned_name.error());
    }
    Array<char> unescaped_description(world.allocator());
    const Expected<std::string_view, Error> description =
        line.word_unquoted(4, unescaped_description);
    if (!description) {
        return make_unexpected(description.error());
    }
    const Expected<WorldText, Error> interned_description = world.intern(*description);
    if (!interned_description) {
        return make_unexpected(interned_description.error());
    }
    WorldFieldDecl declaration;
    declaration.file_field = *file_field;
    declaration.kind = *kind;
    declaration.name = *interned_name;
    declaration.description = *interned_description;
    return world.types().back().fields().push_back(declaration);
}

/// One `field` line of the `type` section.
[[nodiscard]] Status write_field_declaration(const World& world, const WorldFieldDecl& field,
                                             TextWriter& writer) noexcept {
    if (Status began = writer.begin_line(1); !began) {
        return began;
    }
    if (Status word = writer.word("field"); !word) {
        return word;
    }
    if (Status word = writer.word_u64(field.file_field); !word) {
        return word;
    }
    if (Status word = writer.word(world_value_kind_name(field.kind)); !word) {
        return word;
    }
    if (Status word = writer.word_quoted(world.text(field.name)); !word) {
        return word;
    }
    if (Status word = writer.word_quoted(world.text(field.description)); !word) {
        return word;
    }
    return writer.end_line();
}

// --- the content half -----------------------------------------------------------------------

[[nodiscard]] Status read_node_line(const TextLine& line, World& world) noexcept {
    const Expected<u64, Error> position = line.word_u64(1);
    if (!position) {
        return make_unexpected(position.error());
    }
    if (*position != world.nodes().size()) {
        return fail(ErrorCode::InvalidArgument,
                    "a world's nodes are numbered by position and one is out of order");
    }
    u32 parent = WorldNode::kNoParent;
    if (line.word(2) != "-") {
        const Expected<u64, Error> index = line.word_u64(2);
        if (!index) {
            return make_unexpected(index.error());
        }
        if (*index >= world.nodes().size()) {
            return fail(ErrorCode::InvalidArgument, "a world node names a parent written after it");
        }
        parent = static_cast<u32>(*index);
    }
    // ORDINALS ARE POSITIONS PLUS ONE, and that is the whole of the correspondence. See the
    // header: the editor's loader creates nodes in file order from an ordinal counter that starts
    // at one, so this is what it did rather than a convention invented here.
    const Expected<u32, Error> created = world.create_node_with_ordinal(*position + 1U, parent);
    if (!created) {
        return make_unexpected(created.error());
    }
    Array<char> unescaped(world.allocator());
    const Expected<std::string_view, Error> layer = line.word_unquoted(3, unescaped);
    if (!layer) {
        return make_unexpected(layer.error());
    }
    const Expected<WorldText, Error> interned = world.intern(*layer);
    if (!interned) {
        return make_unexpected(interned.error());
    }
    world.nodes()[*created].layer = *interned;
    return ok();
}

[[nodiscard]] Status read_component_line(const TextLine& line, World& world) noexcept {
    if (world.nodes().empty()) {
        return fail(ErrorCode::InvalidArgument, "a world component precedes every node");
    }
    const Expected<u64, Error> file_type = line.word_u64(1);
    if (!file_type) {
        return make_unexpected(file_type.error());
    }
    WorldComponent component(world.allocator());
    component.file_type = *file_type;
    return world.nodes().back().components().push_back(std::move(component));
}

[[nodiscard]] Status read_component_field(const TextLine& line, World& world) noexcept {
    if (world.nodes().empty() || world.nodes().back().components().empty()) {
        return fail(ErrorCode::InvalidArgument, "a world field precedes every component");
    }
    WorldComponent& component = world.nodes().back().components().back();
    const Expected<u64, Error> file_field = line.word_u64(1);
    if (!file_field) {
        return make_unexpected(file_field.error());
    }
    const WorldTypeDecl* declaration = world.type(component.file_type);
    if (declaration == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "a world component names a type the file never declared");
    }
    const WorldFieldDecl* field = declaration->find(*file_field);
    if (field == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "a world component names a field its type never declared");
    }
    WorldField written;
    written.file_field = *file_field;
    if (Status read = read_value(line, 2, field->kind, world, written.value); !read) {
        return read;
    }
    return component.fields().push_back(written);
}

}  // namespace

// --- identity ---------------------------------------------------------------------------------

EditorId editor_document_identity(std::string_view asset_path) noexcept {
    return fnv1a_128(
        Span<const u8>(reinterpret_cast<const u8*>(asset_path.data()), asset_path.size()));
}

EditorId editor_node_identity(const EditorId& document, u64 ordinal) noexcept {
    u8 bytes[24] = {};
    for (u32 index = 0; index < 8; ++index) {
        bytes[index] = static_cast<u8>(document.low >> (index * 8U));
        bytes[index + 8] = static_cast<u8>(document.high >> (index * 8U));
        bytes[index + 16] = static_cast<u8>(ordinal >> (index * 8U));
    }
    return fnv1a_128(Span<const u8>(bytes, sizeof(bytes)));
}

// --- kinds ------------------------------------------------------------------------------------

const char* world_value_kind_name(WorldValueKind kind) noexcept {
    switch (kind) {
        case WorldValueKind::Nil:
            return "nil";
        case WorldValueKind::Bool:
            return "bool";
        case WorldValueKind::Int:
            return "int";
        case WorldValueKind::Float:
            return "float";
        case WorldValueKind::Double:
            return "double";
        case WorldValueKind::Vec2:
            return "vec2";
        case WorldValueKind::Vec3:
            return "vec3";
        case WorldValueKind::Vec4:
            return "vec4";
        case WorldValueKind::Quat:
            return "quat";
        case WorldValueKind::Text:
            return "text";
        case WorldValueKind::Bytes:
            return "bytes";
        case WorldValueKind::Entity:
            return "entity";
    }
    return "bytes";
}

Expected<WorldValueKind, Error> world_value_kind_of(std::string_view name) noexcept {
    static constexpr WorldValueKind kKinds[] = {
        WorldValueKind::Nil,    WorldValueKind::Bool, WorldValueKind::Int,   WorldValueKind::Float,
        WorldValueKind::Double, WorldValueKind::Vec2, WorldValueKind::Vec3,  WorldValueKind::Vec4,
        WorldValueKind::Quat,   WorldValueKind::Text, WorldValueKind::Bytes, WorldValueKind::Entity,
    };
    for (const WorldValueKind kind : kKinds) {
        if (name == world_value_kind_name(kind)) {
            return kind;
        }
    }
    return fail(ErrorCode::InvalidArgument, "a world field names a value kind this build has not");
}

WorldValueKind world_value_kind_of(AuthoringKind kind) noexcept {
    switch (kind) {
        case AuthoringKind::Bool:
            return WorldValueKind::Bool;
        case AuthoringKind::Int:
            return WorldValueKind::Int;
        case AuthoringKind::Float:
            return WorldValueKind::Float;
        case AuthoringKind::Double:
            return WorldValueKind::Double;
        case AuthoringKind::Vec2:
            return WorldValueKind::Vec2;
        case AuthoringKind::Vec3:
            return WorldValueKind::Vec3;
        case AuthoringKind::Vec4:
            return WorldValueKind::Vec4;
        case AuthoringKind::Quat:
            return WorldValueKind::Quat;
        case AuthoringKind::Text:
            return WorldValueKind::Text;
        case AuthoringKind::Entity:
            return WorldValueKind::Entity;
        case AuthoringKind::Bytes:
            break;
    }
    return WorldValueKind::Bytes;
}

// --- the containers -----------------------------------------------------------------------------

WorldField* WorldComponent::find(u64 file_field) noexcept {
    for (WorldField& field : fields_) {
        if (field.file_field == file_field) {
            return &field;
        }
    }
    return nullptr;
}

const WorldField* WorldComponent::find(u64 file_field) const noexcept {
    for (const WorldField& field : fields_) {
        if (field.file_field == file_field) {
            return &field;
        }
    }
    return nullptr;
}

WorldComponent* WorldNode::find(u64 file_type) noexcept {
    for (WorldComponent& component : components_) {
        if (component.file_type == file_type) {
            return &component;
        }
    }
    return nullptr;
}

const WorldComponent* WorldNode::find(u64 file_type) const noexcept {
    for (const WorldComponent& component : components_) {
        if (component.file_type == file_type) {
            return &component;
        }
    }
    return nullptr;
}

const WorldFieldDecl* WorldTypeDecl::find(u64 file_field) const noexcept {
    for (const WorldFieldDecl& field : fields_) {
        if (field.file_field == file_field) {
            return &field;
        }
    }
    return nullptr;
}

Status World::set_path(std::string_view asset_path) noexcept {
    path_.clear();
    if (Status appended = path_.append(Span<const char>(asset_path.data(), asset_path.size()));
        !appended) {
        return appended;
    }
    document_ = editor_document_identity(asset_path);
    reidentify();
    return ok();
}

std::string_view World::text(const WorldText& slice) const noexcept {
    if (static_cast<usize>(slice.offset) + slice.length > text_.size()) {
        return {};
    }
    return {text_.data() + slice.offset, slice.length};
}

Span<const u8> World::blob(const WorldValue& value) const noexcept {
    if (static_cast<usize>(value.blob_offset) + value.blob_length > blobs_.size()) {
        return {};
    }
    return {blobs_.data() + value.blob_offset, value.blob_length};
}

Expected<WorldText, Error> World::intern(std::string_view value) noexcept {
    const WorldText slice{static_cast<u32>(text_.size()), static_cast<u32>(value.size())};
    if (Status appended = text_.append(Span<const char>(value.data(), value.size())); !appended) {
        return make_unexpected(appended.error());
    }
    return slice;
}

Status World::intern_blob(Span<const u8> bytes, WorldValue& value) noexcept {
    value.blob_offset = static_cast<u32>(blobs_.size());
    value.blob_length = static_cast<u32>(bytes.size());
    return blobs_.append(bytes);
}

const WorldTypeDecl* World::type(u64 file_type) const noexcept {
    for (const WorldTypeDecl& declaration : types_) {
        if (declaration.file_type == file_type) {
            return &declaration;
        }
    }
    return nullptr;
}

const WorldTypeDecl* World::type_of(reflect::TypeId engine_type) const noexcept {
    if (!engine_type.valid()) {
        return nullptr;
    }
    for (const WorldTypeDecl& declaration : types_) {
        if (declaration.engine_type == engine_type) {
            return &declaration;
        }
    }
    return nullptr;
}

u32 World::index_of(u64 identity) const noexcept {
    for (usize index = 0; index < nodes_.size(); ++index) {
        if (nodes_[index].live && nodes_[index].identity == identity) {
            return static_cast<u32>(index);
        }
    }
    return WorldNode::kNoParent;
}

Expected<u32, Error> World::create_node(u32 parent) noexcept {
    return create_node_with_ordinal(next_ordinal_, parent);
}

Expected<u32, Error> World::create_node_with_ordinal(u64 ordinal, u32 parent) noexcept {
    WorldNode node(*allocator_);
    node.ordinal = ordinal;
    node.parent = parent;
    node.full_identity = editor_node_identity(document_, ordinal);
    node.identity = node.full_identity.low;
    const u32 index = static_cast<u32>(nodes_.size());
    if (Status pushed = nodes_.push_back(std::move(node)); !pushed) {
        return make_unexpected(pushed.error());
    }
    if (ordinal >= next_ordinal_) {
        next_ordinal_ = ordinal + 1;
    }
    return index;
}

void World::reidentify() noexcept {
    for (WorldNode& node : nodes_) {
        node.full_identity = editor_node_identity(document_, node.ordinal);
        node.identity = node.full_identity.low;
    }
}

// --- reading and writing ------------------------------------------------------------------------

Expected<WorldReadReport, Error> read_world(std::string_view text, std::string_view asset_path,
                                            World& out, WorldReadReport* report) {
    if (Status named = out.set_path(asset_path); !named) {
        return make_unexpected(named.error());
    }
    TextScanner scanner(text);
    const Expected<TextLine, Error> head = scanner.next();
    if (!head) {
        return fail(ErrorCode::InvalidArgument, "a world file is empty");
    }
    if (head->word(0) != kWorldMagic) {
        return fail(ErrorCode::InvalidArgument, "a world file does not begin with 'cyworld'");
    }
    const Expected<u64, Error> version = head->word_u64(1);
    if (!version) {
        return make_unexpected(version.error());
    }
    if (*version > kWorldVersion) {
        return fail(ErrorCode::Unsupported,
                    "a world file is a newer version than this build understands");
    }

    WorldReadReport counted;
    for (;;) {
        const Expected<TextLine, Error> line = scanner.next();
        if (!line) {
            if (line.error().code == ErrorCode::NotFound) {
                break;
            }
            return make_unexpected(line.error());
        }
        const std::string_view keyword = line->word(0);
        // DEPTH IS WHAT SEPARATES THE TWO SECTIONS, exactly as it does on the editor's side: a
        // schema field is written at depth 1 under its type, and a node's field at depth 2 under
        // its component. Dispatching on the keyword alone reads a node's fields as fields of the
        // last declared type, which is a file that loads and is wrong.
        Status handled = ok();
        if (keyword == "type" && line->depth() == 0) {
            handled = read_type_line(*line, out);
            counted.types += 1;
        } else if (keyword == "field" && line->depth() == 1) {
            handled = read_field_line(*line, out);
            counted.fields += 1;
        } else if (keyword == "node" && line->depth() == 0) {
            handled = read_node_line(*line, out);
            counted.nodes += 1;
        } else if (keyword == "component" && line->depth() == 1) {
            handled = read_component_line(*line, out);
            counted.components += 1;
        } else if (keyword == "field" && line->depth() >= 2) {
            handled = read_component_field(*line, out);
        } else {
            return fail(ErrorCode::InvalidArgument,
                        "a world file holds a line this build does not know");
        }
        if (!handled) {
            return make_unexpected(handled.error());
        }
    }
    if (report != nullptr) {
        *report = counted;
    }
    return counted;
}

namespace {

/// The `type` section: every declared type and its fields, in the order they were read.
[[nodiscard]] Status write_type_section(const World& world, TextWriter& writer) noexcept {
    for (const WorldTypeDecl& declaration : world.types()) {
        if (Status began = writer.begin_line(0); !began) {
            return began;
        }
        if (Status word = writer.word("type"); !word) {
            return word;
        }
        if (Status word = writer.word_u64(declaration.file_type); !word) {
            return word;
        }
        if (Status word = writer.word(declaration.authoring_only ? "authoring" : "runtime");
            !word) {
            return word;
        }
        if (Status word = writer.word_quoted(world.text(declaration.name)); !word) {
            return word;
        }
        if (Status ended = writer.end_line(); !ended) {
            return ended;
        }
        for (const WorldFieldDecl& field : declaration.fields()) {
            if (Status written = write_field_declaration(world, field, writer); !written) {
                return written;
            }
        }
    }
    return ok();
}

/// One node's components and their fields, at depth 1 and 2 under it.
[[nodiscard]] Status write_components(const World& world, const WorldNode& node,
                                      TextWriter& writer) noexcept {
    for (const WorldComponent& component : node.components()) {
        if (Status began = writer.begin_line(1); !began) {
            return began;
        }
        if (Status keyword = writer.word("component"); !keyword) {
            return keyword;
        }
        if (Status number = writer.word_u64(component.file_type); !number) {
            return number;
        }
        if (Status ended = writer.end_line(); !ended) {
            return ended;
        }
        for (const WorldField& field : component.fields()) {
            if (Status began = writer.begin_line(2); !began) {
                return began;
            }
            if (Status keyword = writer.word("field"); !keyword) {
                return keyword;
            }
            if (Status number = writer.word_u64(field.file_field); !number) {
                return number;
            }
            if (Status value = write_value(writer, world, field.value); !value) {
                return value;
            }
            if (Status ended = writer.end_line(); !ended) {
                return ended;
            }
        }
    }
    return ok();
}

/// The `node` section.
///
/// POSITIONS ARE RENUMBERED OVER THE LIVE NODES, so a world that had a node deleted writes a dense
/// file — which is what the editor writes and therefore what a round trip has to produce. A deleted
/// node's ordinal is retired rather than reused, so the identities after a save are not the
/// identities before it; that is the editor's rule too, and a load is where both sides agree again.
[[nodiscard]] Status write_node_section(const World& world, TextWriter& writer) noexcept {
    Array<u32> position(world.allocator());
    if (Status sized = position.resize(world.nodes().size()); !sized) {
        return sized;
    }
    u32 next = 0;
    for (usize index = 0; index < world.nodes().size(); ++index) {
        position[index] = world.nodes()[index].live ? next++ : WorldNode::kNoParent;
    }

    for (usize index = 0; index < world.nodes().size(); ++index) {
        const WorldNode& node = world.nodes()[index];
        if (!node.live) {
            continue;
        }
        if (Status began = writer.begin_line(0); !began) {
            return began;
        }
        if (Status word = writer.word("node"); !word) {
            return word;
        }
        if (Status word = writer.word_u64(position[index]); !word) {
            return word;
        }
        const bool parented =
            node.parent != WorldNode::kNoParent && position[node.parent] != WorldNode::kNoParent;
        if (Status word = parented ? writer.word_u64(position[node.parent]) : writer.word("-");
            !word) {
            return word;
        }
        if (Status quoted = writer.word_quoted(world.text(node.layer)); !quoted) {
            return quoted;
        }
        if (Status ended = writer.end_line(); !ended) {
            return ended;
        }
        if (Status written = write_components(world, node, writer); !written) {
            return written;
        }
    }
    return ok();
}

}  // namespace

Status write_world(const World& world, Array<char>& out) noexcept {
    TextWriter writer(out);
    if (Status began = writer.begin_line(0); !began) {
        return began;
    }
    if (Status word = writer.word(kWorldMagic); !word) {
        return word;
    }
    if (Status word = writer.word_u64(kWorldVersion); !word) {
        return word;
    }
    if (Status ended = writer.end_line(); !ended) {
        return ended;
    }
    if (Status written = write_type_section(world, writer); !written) {
        return written;
    }
    return write_node_section(world, writer);
}

Expected<u32, Error> resolve_against(World& world, const AuthoringSchema& schema) noexcept {
    u32 resolved = 0;
    for (WorldTypeDecl& declaration : world.types()) {
        declaration.engine_type = reflect::TypeId{};
        const std::string_view name = world.text(declaration.name);
        const AuthoringType* match = nullptr;
        for (const AuthoringType& candidate : schema.types()) {
            if (candidate.name == name) {
                match = &candidate;
                break;
            }
        }
        if (match == nullptr) {
            // CARRIED, NOT DROPPED. `serialization-and-prefabs`: a build without a plugin must not
            // silently strip that plugin's data from every file it touches.
            for (WorldFieldDecl& field : declaration.fields()) {
                field.engine_field = reflect::FieldId{};
            }
            continue;
        }
        declaration.engine_type = match->id;
        resolved += 1;
        for (WorldFieldDecl& field : declaration.fields()) {
            field.engine_field = reflect::FieldId{};
            const std::string_view field_name = world.text(field.name);
            for (const AuthoringField& candidate : match->fields) {
                if (candidate.name == field_name) {
                    field.engine_field = candidate.id;
                    break;
                }
            }
        }
    }
    for (usize index = 0; index < world.nodes().size(); ++index) {
        for (WorldComponent& component : world.nodes()[index].components()) {
            resolve_component(world, component);
        }
    }
    return resolved;
}

void resolve_component(const World& world, WorldComponent& component) noexcept {
    const WorldTypeDecl* declaration = world.type(component.file_type);
    component.engine_type = (declaration != nullptr) ? declaration->engine_type : reflect::TypeId{};
    for (WorldField& field : component.fields()) {
        const WorldFieldDecl* found =
            (declaration != nullptr) ? declaration->find(field.file_field) : nullptr;
        field.engine_field = (found != nullptr) ? found->engine_field : reflect::FieldId{};
    }
}

bool verify_document_identity(const World& world, const EditorId& claimed) noexcept {
    return world.document() == claimed;
}

namespace {

/// The engine field identifiers a `Transform` component's three parts have here.
struct TransformFields {
    reflect::FieldId rotation;
    reflect::FieldId translation;
    reflect::FieldId scale;
};

[[nodiscard]] const WorldTypeDecl* transform_declaration(const World& world,
                                                         const WorldComponent& component) noexcept {
    if (!component.engine_type.valid()) {
        return nullptr;
    }
    const WorldTypeDecl* declaration = world.type(component.file_type);
    if (declaration == nullptr || world.text(declaration->name) != "Transform") {
        return nullptr;
    }
    return declaration;
}

/// The FILE's field identifiers for the three named parts, found by name through the declaration.
struct TransformSlots {
    u64 rotation = 0;
    u64 translation = 0;
    u64 scale = 0;
    bool complete = false;
};

[[nodiscard]] TransformSlots transform_slots(const World& world,
                                             const WorldTypeDecl& declaration) noexcept {
    TransformSlots slots;
    for (const WorldFieldDecl& field : declaration.fields()) {
        const std::string_view name = world.text(field.name);
        if (name == "rotation") {
            slots.rotation = field.file_field;
        } else if (name == "translation") {
            slots.translation = field.file_field;
        } else if (name == "scale") {
            slots.scale = field.file_field;
        }
    }
    slots.complete = slots.rotation != 0 && slots.translation != 0 && slots.scale != 0;
    return slots;
}

}  // namespace

bool transform_of(const World& world, const WorldNode& node, Transform& out) noexcept {
    for (const WorldComponent& component : node.components()) {
        const WorldTypeDecl* declaration = transform_declaration(world, component);
        if (declaration == nullptr) {
            continue;
        }
        const TransformSlots slots = transform_slots(world, *declaration);
        if (!slots.complete) {
            continue;
        }
        out = Transform::identity();
        if (const WorldField* field = component.find(slots.rotation); field != nullptr) {
            out.rotation = Quat{field->value.lanes[0], field->value.lanes[1], field->value.lanes[2],
                                field->value.lanes[3]};
        }
        if (const WorldField* field = component.find(slots.translation); field != nullptr) {
            out.translation =
                Vec3{field->value.lanes[0], field->value.lanes[1], field->value.lanes[2]};
        }
        if (const WorldField* field = component.find(slots.scale); field != nullptr) {
            out.scale = Vec3{field->value.lanes[0], field->value.lanes[1], field->value.lanes[2]};
        }
        return true;
    }
    return false;
}

bool set_transform(World& world, WorldNode& node, const Transform& placement) noexcept {
    for (WorldComponent& component : node.components()) {
        const WorldTypeDecl* declaration = transform_declaration(world, component);
        if (declaration == nullptr) {
            continue;
        }
        const TransformSlots slots = transform_slots(world, *declaration);
        if (!slots.complete) {
            continue;
        }
        if (WorldField* field = component.find(slots.rotation); field != nullptr) {
            field->value.lanes[0] = placement.rotation.x;
            field->value.lanes[1] = placement.rotation.y;
            field->value.lanes[2] = placement.rotation.z;
            field->value.lanes[3] = placement.rotation.w;
        }
        if (WorldField* field = component.find(slots.translation); field != nullptr) {
            field->value.lanes[0] = placement.translation.x;
            field->value.lanes[1] = placement.translation.y;
            field->value.lanes[2] = placement.translation.z;
        }
        if (WorldField* field = component.find(slots.scale); field != nullptr) {
            field->value.lanes[0] = placement.scale.x;
            field->value.lanes[1] = placement.scale.y;
            field->value.lanes[2] = placement.scale.z;
        }
        return true;
    }
    return false;
}

}  // namespace cy::scene::serialization
