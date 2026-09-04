#include <cy/core/serialize/text.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cy::serialize {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] Status append(Array<char>& out, std::string_view text) noexcept {
    return out.append(Span<const char>(text.data(), text.size()));
}

[[nodiscard]] Status append(Array<char>& out, char value) noexcept {
    return out.push_back(value);
}

/// True when the wire type is written as a signed decimal.
[[nodiscard]] constexpr bool is_signed_wire(WireType wire) noexcept {
    return wire == WireType::I8 || wire == WireType::I16 || wire == WireType::I32 ||
           wire == WireType::I64;
}

/// The wire type whose name is `text`, or `Count` when there is none.
[[nodiscard]] WireType wire_type_from_name(std::string_view text) noexcept {
    for (u8 value = 0; value < static_cast<u8>(WireType::Count); ++value) {
        if (text == wire_type_name(static_cast<WireType>(value))) {
            return static_cast<WireType>(value);
        }
    }
    return WireType::Count;
}

/// Widen a little-endian two's-complement run of `size` bytes to an `i64`.
[[nodiscard]] i64 sign_extend(u64 bits, u32 size) noexcept {
    if (size >= 8) {
        return static_cast<i64>(bits);
    }
    const u32 shift = (8 - size) * 8U;
    return static_cast<i64>(bits << shift) >> shift;
}

[[nodiscard]] u64 little_endian_bits(Span<const u8> bytes) noexcept {
    u64 bits = 0;
    for (usize index = 0; index < bytes.size() && index < 8; ++index) {
        bits |= static_cast<u64>(bytes[index]) << (index * 8U);
    }
    return bits;
}

/// What `snprintf` wrote, or an error when it could not fit.
///
/// One function rather than the same two-part check at four call sites, and the sign narrowing
/// happens once, after the negative case is out of the way.
[[nodiscard]] Expected<usize, Error> written_length(int written, usize capacity) noexcept {
    if (written < 0) {
        return fail(ErrorCode::Internal, "could not format a floating-point value");
    }
    const auto length = static_cast<usize>(written);
    if (length >= capacity) {
        return fail(ErrorCode::BufferTooSmall, "float text buffer too small");
    }
    return length;
}

/// The three values with no decimal form. One spelling each, so a round trip is exact for them too.
[[nodiscard]] Expected<usize, Error> format_special(f64 value, char* out, usize size) noexcept {
    const char* text = "inf";
    if (std::isnan(value)) {
        text = "nan";
    } else if (value < 0) {
        text = "-inf";
    }
    return written_length(std::snprintf(out, size, "%s", text), size);
}

/// Store `size` little-endian bytes of `bits` under `id`.
///
/// The bytes are laid out here rather than by handing `set_scalar` a pointer into a `u64`, because
/// the low `size` bytes of a `u64` are at its front only on a little-endian host — and the whole
/// point of the wire encoding is that the text form does not depend on which host wrote it.
[[nodiscard]] Status set_integer(ValueRecord& record, reflect::FieldId id, WireType wire, u64 bits,
                                 u32 size) noexcept {
    if (size == 0 || size > 8) {
        return fail(ErrorCode::InvalidArgument, "an integer field is one to eight bytes wide");
    }
    u8 payload[8] = {};
    for (u32 index = 0; index < size; ++index) {
        payload[index] = static_cast<u8>((bits >> (index * 8U)) & 0xFFU);
    }
    return record.set(id, wire, payload, size);
}

/// Write the value half of a field line.
[[nodiscard]] Status write_value(Array<char>& out, WireType wire, Span<const u8> bytes) noexcept;

/// Parse the value half of a field line into `record` under `id`.
[[nodiscard]] Status read_value(const TextLine& line, usize index, WireType wire, u32 size,
                                reflect::FieldId id, ValueRecord& record) noexcept;

}  // namespace

Expected<usize, Error> format_f32(f32 value, char* out, usize size) noexcept {
    if (std::isnan(value) || std::isinf(value)) {
        return format_special(static_cast<f64>(value), out, size);
    }
    // Nine significant digits always round-trip an IEEE 754 binary32; the loop is what makes the
    // result the *shortest* that does, so an authored 0.5 stays "0.5" rather than becoming
    // "0.5" only by luck of the format specifier.
    for (int precision = 1; precision <= 9; ++precision) {
        const Expected<usize, Error> length = written_length(
            std::snprintf(out, size, "%.*g", precision, static_cast<f64>(value)), size);
        if (!length) {
            return length;
        }
        if (std::strtof(out, nullptr) == value) {
            return length;
        }
    }
    return fail(ErrorCode::Internal, "no decimal form of this f32 round-trips");
}

Expected<usize, Error> format_f64(f64 value, char* out, usize size) noexcept {
    if (std::isnan(value) || std::isinf(value)) {
        return format_special(value, out, size);
    }
    for (int precision = 1; precision <= 17; ++precision) {
        const Expected<usize, Error> length =
            written_length(std::snprintf(out, size, "%.*g", precision, value), size);
        if (!length) {
            return length;
        }
        if (std::strtod(out, nullptr) == value) {
            return length;
        }
    }
    return fail(ErrorCode::Internal, "no decimal form of this f64 round-trips");
}

// --- TextWriter
// -----------------------------------------------------------------------------------

Status TextWriter::begin_line(u32 depth) noexcept {
    if (line_open_) {
        return fail(ErrorCode::InvalidArgument, "a line is already open");
    }
    for (u32 index = 0; index < depth * kIndentWidth; ++index) {
        if (Status written = append(*out_, ' '); !written) {
            return written;
        }
    }
    line_open_ = true;
    needs_space_ = false;
    return ok();
}

Status TextWriter::word(std::string_view text) noexcept {
    if (!line_open_) {
        return fail(ErrorCode::InvalidArgument, "no line is open");
    }
    if (needs_space_) {
        if (Status written = append(*out_, ' '); !written) {
            return written;
        }
    }
    needs_space_ = true;
    return append(*out_, text);
}

Status TextWriter::word_u64(u64 value) noexcept {
    char text[24] = {};
    const int written =
        std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(value));
    if (written < 0) {
        return fail(ErrorCode::Internal, "could not format an unsigned value");
    }
    return word(std::string_view(text, static_cast<usize>(written)));
}

Status TextWriter::word_i64(i64 value) noexcept {
    char text[24] = {};
    const int written = std::snprintf(text, sizeof(text), "%lld", static_cast<long long>(value));
    if (written < 0) {
        return fail(ErrorCode::Internal, "could not format a signed value");
    }
    return word(std::string_view(text, static_cast<usize>(written)));
}

Status TextWriter::word_quoted(std::string_view text) noexcept {
    if (!line_open_) {
        return fail(ErrorCode::InvalidArgument, "no line is open");
    }
    if (needs_space_) {
        if (Status written = append(*out_, ' '); !written) {
            return written;
        }
    }
    needs_space_ = true;
    if (Status written = append(*out_, '"'); !written) {
        return written;
    }
    for (const char character : text) {
        // Only the two characters that would end the token or start an escape are escaped, plus the
        // control characters that would break the line structure. Everything else — including every
        // multi-byte UTF-8 sequence — goes through unchanged, so a designer's name survives.
        if (character == '"' || character == '\\') {
            if (Status written = append(*out_, '\\'); !written) {
                return written;
            }
            if (Status written = append(*out_, character); !written) {
                return written;
            }
            continue;
        }
        if (character == '\n' || character == '\r' || character == '\t') {
            char escape = 't';
            if (character == '\n') {
                escape = 'n';
            } else if (character == '\r') {
                escape = 'r';
            }
            if (Status written = append(*out_, '\\'); !written) {
                return written;
            }
            if (Status written = append(*out_, escape); !written) {
                return written;
            }
            continue;
        }
        if (Status written = append(*out_, character); !written) {
            return written;
        }
    }
    return append(*out_, '"');
}

Status TextWriter::comment(std::string_view text) noexcept {
    if (!options_.annotate) {
        return ok();
    }
    if (Status written = word("#"); !written) {
        return written;
    }
    return word(text);
}

Status TextWriter::end_line() noexcept {
    if (!line_open_) {
        return fail(ErrorCode::InvalidArgument, "no line is open");
    }
    line_open_ = false;
    needs_space_ = false;
    return append(*out_, '\n');
}

Status TextWriter::write_record(u32 depth, const ValueRecord& record,
                                const reflect::TypeInfo* type) noexcept {
    if (Status opened = begin_line(depth); !opened) {
        return opened;
    }
    if (Status written = word("record"); !written) {
        return written;
    }
    if (Status written = word_u64(record.type().value()); !written) {
        return written;
    }
    if (Status written = word_u64(record.schema_version()); !written) {
        return written;
    }
    if (type != nullptr) {
        if (Status written = comment(type->name); !written) {
            return written;
        }
    }
    if (Status closed = end_line(); !closed) {
        return closed;
    }

    for (const FieldValue& value : record.fields()) {
        if (Status opened = begin_line(depth + 1); !opened) {
            return opened;
        }
        if (Status written = word_u64(value.id.value()); !written) {
            return written;
        }
        if (Status written = word(wire_type_name(value.wire)); !written) {
            return written;
        }
        if (Status written = word_u64(value.size); !written) {
            return written;
        }
        // An empty word writes the separating space and nothing else; the value's own characters
        // are appended straight to the buffer, because they are formatted per wire type rather than
        // being one of the writer's token kinds.
        if (Status written = word(""); !written) {
            return written;
        }
        if (Status written = write_value(*out_, value.wire, record.bytes(value)); !written) {
            return written;
        }
        if (type != nullptr && options_.annotate) {
            const reflect::FieldInfo* field = type->find_field(value.id);
            if (field != nullptr) {
                if (Status written = comment(field->name); !written) {
                    return written;
                }
            }
        }
        if (Status closed = end_line(); !closed) {
            return closed;
        }
    }
    return ok();
}

// --- TextLine and TextScanner
// ---------------------------------------------------------------------

Expected<u64, Error> TextLine::word_u64(usize index) const noexcept {
    const std::string_view text = word(index);
    if (text.empty()) {
        return fail(ErrorCode::InvalidArgument, "expected a number and found nothing");
    }
    u64 value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return fail(ErrorCode::InvalidArgument, "expected an unsigned decimal number");
        }
        const u64 digit = static_cast<u64>(character - '0');
        if (value > (0xFFFF'FFFF'FFFF'FFFFULL - digit) / 10U) {
            return fail(ErrorCode::OutOfRange, "unsigned number does not fit in 64 bits");
        }
        value = (value * 10U) + digit;
    }
    return value;
}

Expected<i64, Error> TextLine::word_i64(usize index) const noexcept {
    std::string_view text = word(index);
    if (text.empty()) {
        return fail(ErrorCode::InvalidArgument, "expected a number and found nothing");
    }
    const bool negative = text.front() == '-';
    if (negative || text.front() == '+') {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return fail(ErrorCode::InvalidArgument, "expected digits after a sign");
    }
    u64 magnitude = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return fail(ErrorCode::InvalidArgument, "expected a signed decimal number");
        }
        magnitude = (magnitude * 10U) + static_cast<u64>(character - '0');
    }
    // The negative extreme has no positive counterpart, so it is compared against its own magnitude
    // rather than against the positive limit.
    const u64 limit = negative ? 0x8000'0000'0000'0000ULL : 0x7FFF'FFFF'FFFF'FFFFULL;
    if (magnitude > limit) {
        return fail(ErrorCode::OutOfRange, "signed number does not fit in 64 bits");
    }
    return negative ? static_cast<i64>(~magnitude + 1U) : static_cast<i64>(magnitude);
}

Expected<std::string_view, Error> TextLine::word_unquoted(usize index,
                                                          Array<char>& out) const noexcept {
    const std::string_view text = word(index);
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
        return fail(ErrorCode::InvalidArgument, "expected a quoted string");
    }
    out.clear();
    for (usize position = 1; position + 1 < text.size(); ++position) {
        char character = text[position];
        if (character == '\\') {
            ++position;
            if (position + 1 >= text.size()) {
                return fail(ErrorCode::InvalidArgument, "a quoted string ends in an escape");
            }
            switch (text[position]) {
                case 'n':
                    character = '\n';
                    break;
                case 'r':
                    character = '\r';
                    break;
                case 't':
                    character = '\t';
                    break;
                case '"':
                case '\\':
                    character = text[position];
                    break;
                default:
                    return fail(ErrorCode::InvalidArgument, "unknown escape in a quoted string");
            }
        }
        if (Status written = out.push_back(character); !written) {
            return make_unexpected(written.error());
        }
    }
    return std::string_view(out.data(), out.size());
}

Expected<TextLine, Error> TextScanner::next() noexcept {
    if (pushed_back_) {
        pushed_back_ = false;
        return current_;
    }
    while (offset_ < text_.size()) {
        const usize start = offset_;
        usize end = text_.find('\n', start);
        if (end == std::string_view::npos) {
            end = text_.size();
            offset_ = text_.size();
        } else {
            offset_ = end + 1;
        }
        ++line_number_;

        std::string_view raw = text_.substr(start, end - start);
        if (!raw.empty() && raw.back() == '\r') {
            raw.remove_suffix(1);
        }

        usize indent = 0;
        while (indent < raw.size() && raw[indent] == ' ') {
            ++indent;
        }
        std::string_view body = raw.substr(indent);
        if (body.empty() || body.front() == '#') {
            continue;
        }

        TextLine line;
        line.depth_ = static_cast<u32>(indent / kIndentWidth);
        line.number_ = line_number_;

        usize position = 0;
        while (position < body.size() && line.count_ < kMaxLineWords) {
            while (position < body.size() && body[position] == ' ') {
                ++position;
            }
            if (position >= body.size() || body[position] == '#') {
                break;
            }
            const usize word_start = position;
            if (body[position] == '"') {
                // A quoted word may contain spaces, so it ends at its closing quote rather than at
                // the next separator. Escapes are honoured here only so that an escaped quote does
                // not end the token early; unescaping is `word_unquoted`'s.
                ++position;
                while (position < body.size() && body[position] != '"') {
                    position += (body[position] == '\\') ? 2 : 1;
                }
                if (position >= body.size()) {
                    return fail(ErrorCode::InvalidArgument, "unterminated quoted string");
                }
                ++position;
            } else {
                while (position < body.size() && body[position] != ' ') {
                    ++position;
                }
            }
            line.words_[line.count_++] = body.substr(word_start, position - word_start);
        }

        current_ = line;
        has_current_ = true;
        return line;
    }
    return fail(ErrorCode::NotFound, "end of text");
}

Status write_value_text(Array<char>& out, WireType wire, Span<const u8> bytes) noexcept {
    return write_value(out, wire, bytes);
}

Status read_value_text(const TextLine& line, usize index, WireType wire, u32 size,
                       Array<u8>& out) noexcept {
    // Routed through a one-field value record rather than through a second parser: the record is
    // where every value's encoding already lives, and a second implementation of "what does this
    // text mean" is a second thing that can disagree with the writer.
    ValueRecord scratch(out.allocator());
    const reflect::FieldId slot(1);
    if (Status read = read_value(line, index, wire, size, slot, scratch); !read) {
        return read;
    }
    out.clear();
    return out.append(scratch.bytes(slot));
}

Status read_record_text(TextScanner& scanner, const TextLine& record_line,
                        ValueRecord& out) noexcept {
    if (record_line.count() < 3 || record_line.word(0) != "record") {
        return fail(ErrorCode::InvalidArgument, "expected a 'record' line");
    }
    const Expected<u64, Error> type = record_line.word_u64(1);
    if (!type) {
        return make_unexpected(type.error());
    }
    const Expected<u64, Error> version = record_line.word_u64(2);
    if (!version) {
        return make_unexpected(version.error());
    }

    out.clear();
    out.set_type(reflect::TypeId(static_cast<u32>(type.value())));
    out.set_schema_version(static_cast<u16>(version.value()));

    const u32 field_depth = record_line.depth() + 1;
    while (true) {
        const Expected<TextLine, Error> line = scanner.next();
        if (!line) {
            break;  // End of input closes the record, exactly as a shallower line would.
        }
        if (line->depth() < field_depth) {
            scanner.push_back();
            break;
        }
        if (line->count() < 4) {
            return fail(ErrorCode::InvalidArgument,
                        "a field line is '<field_id> <wire> <bytes> <value>'");
        }
        const Expected<u64, Error> id = line->word_u64(0);
        if (!id) {
            return make_unexpected(id.error());
        }
        const WireType wire = wire_type_from_name(line->word(1));
        if (wire == WireType::Count) {
            return fail(ErrorCode::Unsupported, "unknown wire type in the text form");
        }
        const Expected<u64, Error> size = line->word_u64(2);
        if (!size) {
            return make_unexpected(size.error());
        }
        if (Status read = read_value(*line, 3, wire, static_cast<u32>(size.value()),
                                     reflect::FieldId(static_cast<u32>(id.value())), out);
            !read) {
            return read;
        }
    }
    return ok();
}

namespace {

Status write_value(Array<char>& out, WireType wire, Span<const u8> bytes) noexcept {
    switch (wire) {
        case WireType::Bool:
            return append(out, bytes.empty() || bytes[0] == 0 ? "false" : "true");
        case WireType::I8:
        case WireType::I16:
        case WireType::I32:
        case WireType::I64: {
            char text[24] = {};
            const i64 value =
                sign_extend(little_endian_bits(bytes), static_cast<u32>(bytes.size()));
            const int written =
                std::snprintf(text, sizeof(text), "%lld", static_cast<long long>(value));
            return append(out, std::string_view(text, static_cast<usize>(written)));
        }
        case WireType::U8:
        case WireType::U16:
        case WireType::U32:
        case WireType::U64:
        case WireType::Enum:
        case WireType::Flags: {
            char text[24] = {};
            const int written =
                std::snprintf(text, sizeof(text), "%llu",
                              static_cast<unsigned long long>(little_endian_bits(bytes)));
            return append(out, std::string_view(text, static_cast<usize>(written)));
        }
        case WireType::F32: {
            f32 value = 0.0F;
            if (Status decoded = decode_scalar(WireType::F32, bytes.data(),
                                               static_cast<u32>(bytes.size()), &value);
                !decoded) {
                return decoded;
            }
            char text[kFloatTextCapacity] = {};
            const Expected<usize, Error> written = format_f32(value, text, sizeof(text));
            if (!written) {
                return make_unexpected(written.error());
            }
            return append(out, std::string_view(text, written.value()));
        }
        case WireType::F64: {
            f64 value = 0.0;
            if (Status decoded = decode_scalar(WireType::F64, bytes.data(),
                                               static_cast<u32>(bytes.size()), &value);
                !decoded) {
                return decoded;
            }
            char text[kFloatTextCapacity] = {};
            const Expected<usize, Error> written = format_f64(value, text, sizeof(text));
            if (!written) {
                return make_unexpected(written.error());
            }
            return append(out, std::string_view(text, written.value()));
        }
        case WireType::LocalRef: {
            char text[24] = {};
            const int written =
                std::snprintf(text, sizeof(text), "@%llu",
                              static_cast<unsigned long long>(little_endian_bits(bytes)));
            return append(out, std::string_view(text, static_cast<usize>(written)));
        }
        case WireType::ExternalRef: {
            if (bytes.size() != 20) {
                return fail(ErrorCode::InvalidArgument, "an external reference is twenty bytes");
            }
            for (usize index = 0; index < 16; ++index) {
                const u8 byte = bytes[index];
                if (Status written = append(out, kHexDigits[byte >> 4U]); !written) {
                    return written;
                }
                if (Status written = append(out, kHexDigits[byte & 0x0FU]); !written) {
                    return written;
                }
            }
            char text[24] = {};
            const u32 local = static_cast<u32>(little_endian_bits(bytes.subspan(16, 4)));
            const int written =
                std::snprintf(text, sizeof(text), "@%lu", static_cast<unsigned long>(local));
            return append(out, std::string_view(text, static_cast<usize>(written)));
        }
        case WireType::Bytes:
        case WireType::Count:
            break;
    }
    if (bytes.empty()) {
        return append(out, '-');
    }
    for (const u8 byte : bytes) {
        if (Status written = append(out, kHexDigits[byte >> 4U]); !written) {
            return written;
        }
        if (Status written = append(out, kHexDigits[byte & 0x0FU]); !written) {
            return written;
        }
    }
    return ok();
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

/// Decode `2 * count` hex characters at `text` into `out`.
[[nodiscard]] Status read_hex(std::string_view text, u8* out, usize count) noexcept {
    if (text.size() < count * 2) {
        return fail(ErrorCode::InvalidArgument, "hexadecimal run is shorter than declared");
    }
    for (usize index = 0; index < count; ++index) {
        const Expected<u8, Error> high = hex_value(text[index * 2]);
        if (!high) {
            return make_unexpected(high.error());
        }
        const Expected<u8, Error> low = hex_value(text[(index * 2) + 1]);
        if (!low) {
            return make_unexpected(low.error());
        }
        out[index] = static_cast<u8>((high.value() << 4U) | low.value());
    }
    return ok();
}

/// A run of decimal digits as a number, with no sign and no leading zero rule.
///
/// Used for the tail of a reference, where `word_u64` cannot be: a reference's text carries a
/// prefix, so the number is a substring rather than a whole word.
[[nodiscard]] Expected<u64, Error> decimal(std::string_view text) noexcept {
    if (text.empty()) {
        return fail(ErrorCode::InvalidArgument, "a local id is a decimal number");
    }
    u64 value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return fail(ErrorCode::InvalidArgument, "a local id is a decimal number");
        }
        value = (value * 10U) + static_cast<u64>(character - '0');
    }
    return value;
}

/// `strtof`/`strtod` need a terminated string and a `TextLine`'s words are views, so the digits are
/// copied into a buffer wide enough for any float either writer produces.
[[nodiscard]] Status read_float(std::string_view text, WireType wire, reflect::FieldId id,
                                ValueRecord& record) noexcept {
    char terminated[kFloatTextCapacity] = {};
    if (text.size() >= sizeof(terminated)) {
        return fail(ErrorCode::OutOfRange, "float text is longer than any float needs");
    }
    std::memcpy(terminated, text.data(), text.size());
    if (wire == WireType::F32) {
        const f32 value = std::strtof(terminated, nullptr);
        return record.set_scalar(id, wire, &value, sizeof(value));
    }
    const f64 value = std::strtod(terminated, nullptr);
    return record.set_scalar(id, wire, &value, sizeof(value));
}

[[nodiscard]] Status read_local_reference(std::string_view text, reflect::FieldId id,
                                          ValueRecord& record) noexcept {
    if (text.empty() || text.front() != '@') {
        return fail(ErrorCode::InvalidArgument, "a local reference is '@<local id>'");
    }
    const Expected<u64, Error> local = decimal(text.substr(1));
    if (!local) {
        return make_unexpected(local.error());
    }
    return record.set_local_reference(id, static_cast<u32>(local.value()));
}

[[nodiscard]] Status read_external_reference(std::string_view text, reflect::FieldId id,
                                             ValueRecord& record) noexcept {
    constexpr usize kAssetHexLength = 32;
    if (text.find('@') != kAssetHexLength) {
        return fail(ErrorCode::InvalidArgument,
                    "an external reference is 32 hex digits then '@<local id>'");
    }
    u8 payload[20] = {};
    if (Status read = read_hex(text.substr(0, kAssetHexLength), payload, 16); !read) {
        return read;
    }
    const Expected<u64, Error> local = decimal(text.substr(kAssetHexLength + 1));
    if (!local) {
        return make_unexpected(local.error());
    }
    for (u32 byte = 0; byte < 4; ++byte) {
        payload[16 + byte] = static_cast<u8>((local.value() >> (byte * 8U)) & 0xFFU);
    }
    return record.set(id, WireType::ExternalRef, payload, sizeof(payload));
}

/// An opaque run: `-` for empty, lowercase hex otherwise. Also where a wire type this reader does
/// not interpret ends up, which is why it copies rather than parses.
[[nodiscard]] Status read_bytes_value(std::string_view text, u32 size, reflect::FieldId id,
                                      ValueRecord& record) noexcept {
    if (text == "-") {
        return record.set(id, WireType::Bytes, nullptr, 0);
    }
    Array<u8> payload(record.allocator());
    if (Status sized = payload.resize(size); !sized) {
        return sized;
    }
    if (Status read = read_hex(text, payload.data(), size); !read) {
        return read;
    }
    return record.set(id, WireType::Bytes, payload.data(), size);
}

Status read_value(const TextLine& line, usize index, WireType wire, u32 size, reflect::FieldId id,
                  ValueRecord& record) noexcept {
    const std::string_view text = line.word(index);
    switch (wire) {
        case WireType::Bool: {
            if (text != "true" && text != "false") {
                return fail(ErrorCode::InvalidArgument, "a Bool is 'true' or 'false'");
            }
            const u8 value = (text == "true") ? 1U : 0U;
            return record.set(id, wire, &value, 1);
        }
        case WireType::I8:
        case WireType::I16:
        case WireType::I32:
        case WireType::I64: {
            const Expected<i64, Error> value = line.word_i64(index);
            if (!value) {
                return make_unexpected(value.error());
            }
            return set_integer(record, id, wire, static_cast<u64>(value.value()), size);
        }
        case WireType::U8:
        case WireType::U16:
        case WireType::U32:
        case WireType::U64:
        case WireType::Enum:
        case WireType::Flags: {
            const Expected<u64, Error> value = line.word_u64(index);
            if (!value) {
                return make_unexpected(value.error());
            }
            return set_integer(record, id, wire, value.value(), size);
        }
        case WireType::F32:
        case WireType::F64:
            return read_float(text, wire, id, record);
        case WireType::LocalRef:
            return read_local_reference(text, id, record);
        case WireType::ExternalRef:
            return read_external_reference(text, id, record);
        case WireType::Bytes:
        case WireType::Count:
            break;
    }
    return read_bytes_value(text, size, id, record);
}

}  // namespace
}  // namespace cy::serialize
