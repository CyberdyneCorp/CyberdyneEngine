// The binary envelope and the text form. Task 3.3.5.

#include <cy/core/assets/serialization.h>

#include <cy/core/base/assert.h>

#include <algorithm>
#include <bit>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace cy::assets {
namespace {

// The format is little-endian by definition. On a big-endian host the record's own byte-by-byte
// encoding would still be correct, but nothing here has ever run on one, so it is refused at
// compile time rather than claimed.
static_assert(std::endian::native == std::endian::little,
              "the binary format is little-endian and this engine has only been built for a "
              "little-endian host; a big-endian port must swap in reflect's record encoding");

/// The byte-order mark, written into the envelope. A reader that finds it byte-swapped knows it is
/// looking at a foreign file rather than at a corrupt one.
constexpr u32 kByteOrderMark = 0x0102'0304u;

void write_u32(u8* out, u32 value) noexcept {
    for (usize i = 0; i < 4; ++i) {
        out[i] = static_cast<u8>(value >> (i * 8));
    }
}

[[nodiscard]] u32 read_u32(const u8* in) noexcept {
    u32 value = 0;
    for (usize i = 0; i < 4; ++i) {
        value |= static_cast<u32>(in[i]) << (i * 8);
    }
    return value;
}

/// Validate the envelope and report where the record starts.
Status check_envelope(const u8* data, usize size) noexcept {
    if (data == nullptr || size < kBinaryEnvelopeBytes) {
        return fail(ErrorCode::InvalidArgument, "the document is shorter than its envelope");
    }
    if (std::memcmp(data, kBinaryMagic, sizeof(kBinaryMagic)) != 0) {
        return fail(ErrorCode::InvalidArgument,
                    "the document does not begin with the binary magic");
    }
    const u32 version = read_u32(data + 8);
    if (version != kBinaryFormatVersion) {
        return fail(ErrorCode::Unsupported,
                    "the document's format version is newer than this engine reads");
    }
    if (read_u32(data + 12) != kByteOrderMark) {
        return fail(ErrorCode::Unsupported,
                    "the document was written by a host of the opposite byte order");
    }
    return ok();
}

/// The fields of a type, ordered by identifier ascending, with Transient ones dropped.
///
/// One helper for both the writer and the reader's diagnostics, because the ORDER is the whole
/// diff-friendliness argument and two copies of it would eventually differ.
std::vector<const reflect::FieldInfo*> ordered_fields(const reflect::TypeInfo& type) {
    std::vector<const reflect::FieldInfo*> fields;
    fields.reserve(type.field_count);
    for (u32 index = 0; index < type.field_count; ++index) {
        const reflect::FieldInfo& field = type.fields[index];
        // Transient fields are excluded from serialization by `core-type-system`, and reflect's
        // binary record already skips them; the text form must skip the same ones or the two forms
        // would not round-trip into each other.
        if (field.attributes.transient()) {
            continue;
        }
        fields.push_back(&field);
    }
    std::ranges::sort(fields, [](const reflect::FieldInfo* a, const reflect::FieldInfo* b) {
        return a->id.value() < b->id.value();
    });
    return fields;
}

/// Render one scalar field's value. Integers are decimal; floats use %.9g / %.17g, the shortest
/// forms that round-trip an f32 and an f64 exactly, so text → binary → text is value-preserving
/// rather than nearly so.
int format_value(char* out, usize capacity, const reflect::FieldInfo& field,
                 const void* object) noexcept {
    const auto* base = static_cast<const u8*>(object) + field.offset;
    switch (field.kind) {
        case reflect::FieldKind::Bool: {
            bool value = false;
            std::memcpy(&value, base, sizeof(value));
            return std::snprintf(out, capacity, "%s", value ? "true" : "false");
        }
        case reflect::FieldKind::F32: {
            f32 value = 0;
            std::memcpy(&value, base, sizeof(value));
            return std::snprintf(out, capacity, "%.9g", static_cast<f64>(value));
        }
        case reflect::FieldKind::F64: {
            f64 value = 0;
            std::memcpy(&value, base, sizeof(value));
            return std::snprintf(out, capacity, "%.17g", value);
        }
        case reflect::FieldKind::I8:
        case reflect::FieldKind::I16:
        case reflect::FieldKind::I32:
        case reflect::FieldKind::I64: {
            i64 value = 0;
            std::memcpy(&value, base, field.size);
            // Sign-extend from the field's own width: the memcpy above filled only `size` bytes.
            const u32 shift = 64 - (field.size * 8);
            value = (value << shift) >> shift;
            return std::snprintf(out, capacity, "%" PRId64, value);
        }
        case reflect::FieldKind::U8:
        case reflect::FieldKind::U16:
        case reflect::FieldKind::U32:
        case reflect::FieldKind::U64:
        case reflect::FieldKind::Enum:
        case reflect::FieldKind::Flags: {
            u64 value = 0;
            std::memcpy(&value, base, field.size);
            return std::snprintf(out, capacity, "%" PRIu64, value);
        }
        case reflect::FieldKind::Unsupported:
            break;
    }
    return -1;
}

/// Apply one rendered value to a field. Returns false when the text is not a value of that kind.
bool apply_value(const reflect::FieldInfo& field, std::string_view text, void* object) noexcept {
    auto* base = static_cast<u8*>(object) + field.offset;
    // strtoll and strtod need a terminated buffer; a field value is short by construction.
    char buffer[64] = {};
    if (text.size() >= sizeof(buffer)) {
        return false;
    }
    std::memcpy(buffer, text.data(), text.size());

    char* end = nullptr;
    switch (field.kind) {
        case reflect::FieldKind::Bool: {
            const bool value = text == "true";
            if (!value && text != "false") {
                return false;
            }
            std::memcpy(base, &value, sizeof(value));
            return true;
        }
        case reflect::FieldKind::F32: {
            const f32 value = std::strtof(buffer, &end);
            if (end == buffer || *end != '\0') {
                return false;
            }
            std::memcpy(base, &value, sizeof(value));
            return true;
        }
        case reflect::FieldKind::F64: {
            const f64 value = std::strtod(buffer, &end);
            if (end == buffer || *end != '\0') {
                return false;
            }
            std::memcpy(base, &value, sizeof(value));
            return true;
        }
        case reflect::FieldKind::I8:
        case reflect::FieldKind::I16:
        case reflect::FieldKind::I32:
        case reflect::FieldKind::I64: {
            const long long value = std::strtoll(buffer, &end, 10);
            if (end == buffer || *end != '\0') {
                return false;
            }
            const auto widened = static_cast<i64>(value);
            std::memcpy(base, &widened, field.size);
            return true;
        }
        case reflect::FieldKind::U8:
        case reflect::FieldKind::U16:
        case reflect::FieldKind::U32:
        case reflect::FieldKind::U64:
        case reflect::FieldKind::Enum:
        case reflect::FieldKind::Flags: {
            const unsigned long long value = std::strtoull(buffer, &end, 10);
            if (end == buffer || *end != '\0') {
                return false;
            }
            const auto widened = static_cast<u64>(value);
            std::memcpy(base, &widened, field.size);
            return true;
        }
        case reflect::FieldKind::Unsupported:
            break;
    }
    return false;
}

std::string_view trim(std::string_view text) noexcept {
    usize begin = 0;
    usize end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    while (end > begin &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

}  // namespace

// --- Binary ------------------------------------------------------------------------------------

Status write_binary(const reflect::TypeInfo& type, const void* object,
                    reflect::ByteBuffer& out) noexcept {
    u8 envelope[kBinaryEnvelopeBytes] = {};
    std::memcpy(envelope, kBinaryMagic, sizeof(kBinaryMagic));
    write_u32(envelope + 8, kBinaryFormatVersion);
    write_u32(envelope + 12, kByteOrderMark);
    if (Status written = out.append(envelope, sizeof(envelope)); !written) {
        return written;
    }
    return reflect::write_record(type, object, out);
}

Status read_binary(const reflect::TypeInfo& type, const u8* data, usize size,
                   void* object) noexcept {
    if (Status envelope = check_envelope(data, size); !envelope) {
        return envelope;
    }
    return reflect::read_record(type, data + kBinaryEnvelopeBytes, size - kBinaryEnvelopeBytes,
                                object);
}

Expected<reflect::TypeId, Error> peek_binary(const u8* data, usize size) noexcept {
    if (Status envelope = check_envelope(data, size); !envelope) {
        return make_unexpected(envelope.error());
    }
    Expected<reflect::RecordHeader, Error> header =
        reflect::peek_record(data + kBinaryEnvelopeBytes, size - kBinaryEnvelopeBytes);
    if (!header) {
        return make_unexpected(header.error());
    }
    return header.value().type;
}

// --- Text --------------------------------------------------------------------------------------

Status write_text(const reflect::TypeInfo& type, const void* object, Array<char>& out) noexcept {
    out.clear();

    const auto append = [&out](const char* text, usize length) noexcept -> Status {
        const usize base = out.size();
        if (Status grown = out.resize(base + length); !grown) {
            return grown;
        }
        std::memcpy(out.data() + base, text, length);
        return ok();
    };

    char line[512] = {};
    int written = std::snprintf(line, sizeof(line), "type \"%s\" %u\n", type.name, type.id.value());
    if (written < 0 || static_cast<usize>(written) >= sizeof(line)) {
        return fail(ErrorCode::BufferTooSmall, "the type header line does not fit");
    }
    if (Status appended = append(line, static_cast<usize>(written)); !appended) {
        return appended;
    }

    for (const reflect::FieldInfo* field : ordered_fields(type)) {
        char value[64] = {};
        const int rendered = format_value(value, sizeof(value), *field, object);
        if (rendered < 0 || static_cast<usize>(rendered) >= sizeof(value)) {
            return fail(ErrorCode::Unsupported,
                        "a field's kind has no text form; strings, containers and nested reflected "
                        "structs arrive with the values module's Var");
        }
        written = std::snprintf(line, sizeof(line), "  %u %s = %s\n", field->id.value(),
                                field->name, value);
        if (written < 0 || static_cast<usize>(written) >= sizeof(line)) {
            return fail(ErrorCode::BufferTooSmall, "a field line does not fit");
        }
        if (Status appended = append(line, static_cast<usize>(written)); !appended) {
            return appended;
        }
    }

    // NUL-terminated but not counted: `out.size()` is the document's length, and `out.data()` may
    // be handed to anything that wants a C string.
    if (Status grown = out.resize(out.size() + 1); !grown) {
        return grown;
    }
    out[out.size() - 1] = '\0';
    out.pop_back();
    return ok();
}

namespace {

/// The header line, shared by `read_text` and `peek_text`.
Expected<reflect::TypeId, Error> parse_header(std::string_view line) noexcept {
    const std::string_view trimmed = trim(line);
    if (trimmed.size() < 6 || !trimmed.starts_with("type ")) {
        return fail(ErrorCode::InvalidArgument, "the document does not begin with a type line");
    }
    const usize open_quote = trimmed.find('"');
    const usize close_quote = open_quote == std::string_view::npos
                                  ? std::string_view::npos
                                  : trimmed.find('"', open_quote + 1);
    if (close_quote == std::string_view::npos) {
        return fail(ErrorCode::InvalidArgument, "the type line has no quoted type name");
    }
    const std::string_view id_text = trim(trimmed.substr(close_quote + 1));
    char buffer[16] = {};
    if (id_text.empty() || id_text.size() >= sizeof(buffer)) {
        return fail(ErrorCode::InvalidArgument, "the type line has no type identifier");
    }
    std::memcpy(buffer, id_text.data(), id_text.size());
    char* end = nullptr;
    const unsigned long value = std::strtoul(buffer, &end, 10);
    if (end == buffer || *end != '\0') {
        return fail(ErrorCode::InvalidArgument, "the type line's identifier is not a number");
    }
    return reflect::TypeId(static_cast<u32>(value));
}

}  // namespace

Expected<reflect::TypeId, Error> peek_text(std::string_view text) noexcept {
    const usize newline = text.find('\n');
    return parse_header(text.substr(0, newline == std::string_view::npos ? text.size() : newline));
}

Status read_text(const reflect::TypeInfo& type, std::string_view text, void* object) noexcept {
    usize cursor = 0;
    bool have_header = false;

    while (cursor <= text.size()) {
        const usize newline = text.find('\n', cursor);
        const usize end = newline == std::string_view::npos ? text.size() : newline;
        const std::string_view line = trim(text.substr(cursor, end - cursor));
        cursor = end + 1;

        if (!line.empty() && line.front() != '#') {
            if (!have_header) {
                Expected<reflect::TypeId, Error> declared = parse_header(line);
                if (!declared) {
                    return make_unexpected(declared.error());
                }
                if (declared.value() != type.id) {
                    return fail(ErrorCode::InvalidArgument,
                                "the document declares a different type identifier than the type "
                                "it is being read into");
                }
                have_header = true;
            } else {
                // `<field id> <name> = <value>`. The name is not consulted: identity is the id.
                const usize equals = line.find('=');
                const usize space = line.find(' ');
                if (equals == std::string_view::npos || space == std::string_view::npos ||
                    space > equals) {
                    return fail(ErrorCode::InvalidArgument,
                                "a field line is not `<id> <name> = <value>`");
                }
                char id_buffer[16] = {};
                const std::string_view id_text = trim(line.substr(0, space));
                if (id_text.empty() || id_text.size() >= sizeof(id_buffer)) {
                    return fail(ErrorCode::InvalidArgument, "a field line has no identifier");
                }
                std::memcpy(id_buffer, id_text.data(), id_text.size());
                char* id_end = nullptr;
                const unsigned long id_value = std::strtoul(id_buffer, &id_end, 10);
                if (id_end == id_buffer || *id_end != '\0') {
                    return fail(ErrorCode::InvalidArgument,
                                "a field line's identifier is not a number");
                }

                const reflect::FieldInfo* field =
                    type.find_field(reflect::FieldId(static_cast<u32>(id_value)));
                if (field != nullptr && !field->attributes.transient()) {
                    if (!apply_value(*field, trim(line.substr(equals + 1)), object)) {
                        return fail(ErrorCode::InvalidArgument,
                                    "a field's value is not a value of that field's kind");
                    }
                }
                // A field the type no longer has is skipped: that is a removed field, and the
                // manifest's tombstone is what makes skipping it safe rather than lossy.
            }
        }

        if (newline == std::string_view::npos) {
            break;
        }
    }

    if (!have_header) {
        return fail(ErrorCode::InvalidArgument, "the document has no type line");
    }
    return ok();
}

}  // namespace cy::assets
