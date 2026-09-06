#include <cy/import/json.h>

#include <cstdlib>
#include <cstring>

#include <vector>

namespace cy::import {
namespace {

[[nodiscard]] bool is_space(char character) noexcept {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

[[nodiscard]] bool is_digit(char character) noexcept {
    return character >= '0' && character <= '9';
}

/// One UTF-16 code unit's worth of `\uXXXX`, or -1.
[[nodiscard]] i32 hex_quad(std::string_view text, usize at) noexcept {
    if (at + 4 > text.size()) {
        return -1;
    }
    i32 value = 0;
    for (usize index = 0; index < 4; ++index) {
        const char character = text[at + index];
        i32 digit = 0;
        if (character >= '0' && character <= '9') {
            digit = character - '0';
        } else if (character >= 'a' && character <= 'f') {
            digit = (character - 'a') + 10;
        } else if (character >= 'A' && character <= 'F') {
            digit = (character - 'A') + 10;
        } else {
            return -1;
        }
        value = (value * 16) + digit;
    }
    return value;
}

}  // namespace

/// The recursive-descent parser. A separate class so that the document itself has no parsing state
/// and a caller cannot half-parse one.
class JsonDocument::Parser {
public:
    Parser(JsonDocument& document, std::string_view text) noexcept
        : document_(document), text_(text) {}

    [[nodiscard]] Status run() noexcept {
        skip_space();
        Expected<JsonRef, Error> value = parse_value(0);
        if (!value) {
            return make_unexpected(value.error());
        }
        skip_space();
        if (cursor_ != text_.size()) {
            // Anything after the root is refused rather than ignored. A file with two documents in
            // it means something different to every parser that accepts it.
            return fail(ErrorCode::InvalidArgument, "trailing content after the JSON root value");
        }
        return ok();
    }

private:
    void skip_space() noexcept {
        while (cursor_ < text_.size() && is_space(text_[cursor_])) {
            ++cursor_;
        }
    }

    [[nodiscard]] bool consume(char expected) noexcept {
        if (cursor_ < text_.size() && text_[cursor_] == expected) {
            ++cursor_;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool literal(std::string_view expected) noexcept {
        if (text_.substr(cursor_, expected.size()) != expected) {
            return false;
        }
        cursor_ += expected.size();
        return true;
    }

    [[nodiscard]] Expected<JsonRef, Error> add(const Node& node) noexcept {
        if (Status pushed = document_.values_.push_back(node); !pushed) {
            return make_unexpected(pushed.error());
        }
        return static_cast<JsonRef>(document_.values_.size() - 1);
    }

    /// Copy `text` into the document's blob and return its slice.
    [[nodiscard]] Expected<u32, Error> intern(std::string_view text, u32& out_length) noexcept {
        const auto offset = static_cast<u32>(document_.text_.size());
        if (Status appended = document_.text_.append(Span<const char>(text.data(), text.size()));
            !appended) {
            return make_unexpected(appended.error());
        }
        out_length = static_cast<u32>(text.size());
        return offset;
    }

    /// Read a string, decoding its escapes into `scratch_`.
    [[nodiscard]] Status parse_string(u32& out_offset, u32& out_length) noexcept {
        if (!consume('"')) {
            return fail(ErrorCode::InvalidArgument, "expected a string");
        }
        scratch_.clear();
        while (true) {
            if (cursor_ >= text_.size()) {
                return fail(ErrorCode::InvalidArgument, "a string that is never closed");
            }
            const char character = text_[cursor_++];
            if (character == '"') {
                break;
            }
            if (static_cast<unsigned char>(character) < 0x20U) {
                return fail(ErrorCode::InvalidArgument,
                            "a raw control character inside a string; escape it");
            }
            if (character != '\\') {
                scratch_.push_back(character);
                continue;
            }
            if (cursor_ >= text_.size()) {
                return fail(ErrorCode::InvalidArgument, "a string that ends inside an escape");
            }
            const char escape = text_[cursor_++];
            switch (escape) {
                case '"':
                    scratch_.push_back('"');
                    break;
                case '\\':
                    scratch_.push_back('\\');
                    break;
                case '/':
                    scratch_.push_back('/');
                    break;
                case 'b':
                    scratch_.push_back('\b');
                    break;
                case 'f':
                    scratch_.push_back('\f');
                    break;
                case 'n':
                    scratch_.push_back('\n');
                    break;
                case 'r':
                    scratch_.push_back('\r');
                    break;
                case 't':
                    scratch_.push_back('\t');
                    break;
                case 'u': {
                    const i32 unit = hex_quad(text_, cursor_);
                    if (unit < 0) {
                        return fail(ErrorCode::InvalidArgument,
                                    "a \\u escape that is not four hex digits");
                    }
                    cursor_ += 4;
                    u32 codepoint = static_cast<u32>(unit);
                    if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                        // A surrogate pair. Its low half must follow, or the string names no
                        // character at all — which is worth refusing rather than emitting U+FFFD
                        // and letting a file name become subtly wrong.
                        if (cursor_ + 6 > text_.size() || text_[cursor_] != '\\' ||
                            text_[cursor_ + 1] != 'u') {
                            return fail(ErrorCode::InvalidArgument,
                                        "a high surrogate with no low surrogate after it");
                        }
                        const i32 low = hex_quad(text_, cursor_ + 2);
                        if (low < 0xDC00 || low > 0xDFFF) {
                            return fail(ErrorCode::InvalidArgument,
                                        "a high surrogate followed by something else");
                        }
                        cursor_ += 6;
                        codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) +
                                    (static_cast<u32>(low) - 0xDC00U);
                    }
                    // UTF-8, written out rather than delegated: the standard library's conversions
                    // are locale-sensitive or deprecated, and this is four branches.
                    if (codepoint < 0x80U) {
                        scratch_.push_back(static_cast<char>(codepoint));
                    } else if (codepoint < 0x800U) {
                        scratch_.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
                        scratch_.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
                    } else if (codepoint < 0x10000U) {
                        scratch_.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
                        scratch_.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
                        scratch_.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
                    } else {
                        scratch_.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
                        scratch_.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
                        scratch_.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
                        scratch_.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
                    }
                    break;
                }
                default:
                    return fail(ErrorCode::InvalidArgument, "an escape JSON does not define");
            }
        }
        Expected<u32, Error> offset =
            intern(std::string_view(scratch_.data(), scratch_.size()), out_length);
        if (!offset) {
            return make_unexpected(offset.error());
        }
        out_offset = offset.value();
        return ok();
    }

    [[nodiscard]] Expected<JsonRef, Error> parse_number() noexcept {
        const usize start = cursor_;
        if (cursor_ < text_.size() && text_[cursor_] == '-') {
            ++cursor_;
        }
        // JSON forbids a leading zero followed by digits, and forbids `.5` and `5.`. Enforced
        // rather than left to strtod, which accepts all three and would make this parser disagree
        // with every other one about what a file says.
        if (cursor_ >= text_.size() || !is_digit(text_[cursor_])) {
            return fail(ErrorCode::InvalidArgument, "a number with no digits");
        }
        if (text_[cursor_] == '0') {
            ++cursor_;
        } else {
            while (cursor_ < text_.size() && is_digit(text_[cursor_])) {
                ++cursor_;
            }
        }
        if (cursor_ < text_.size() && text_[cursor_] == '.') {
            ++cursor_;
            if (cursor_ >= text_.size() || !is_digit(text_[cursor_])) {
                return fail(ErrorCode::InvalidArgument, "a decimal point with no digits after it");
            }
            while (cursor_ < text_.size() && is_digit(text_[cursor_])) {
                ++cursor_;
            }
        }
        if (cursor_ < text_.size() && (text_[cursor_] == 'e' || text_[cursor_] == 'E')) {
            ++cursor_;
            if (cursor_ < text_.size() && (text_[cursor_] == '+' || text_[cursor_] == '-')) {
                ++cursor_;
            }
            if (cursor_ >= text_.size() || !is_digit(text_[cursor_])) {
                return fail(ErrorCode::InvalidArgument, "an exponent with no digits");
            }
            while (cursor_ < text_.size() && is_digit(text_[cursor_])) {
                ++cursor_;
            }
        }

        char buffer[64] = {};
        const usize length = cursor_ - start;
        if (length == 0 || length >= sizeof(buffer)) {
            return fail(ErrorCode::InvalidArgument, "a number longer than any real value");
        }
        std::memcpy(buffer, text_.data() + start, length);
        Node node;
        node.kind = JsonKind::Number;
        node.number = std::strtod(buffer, nullptr);
        return add(node);
    }

    [[nodiscard]] Expected<JsonRef, Error> parse_array(u32 depth) noexcept {
        if (!consume('[')) {
            return fail(ErrorCode::InvalidArgument, "expected an array");
        }
        std::vector<Child> collected;
        skip_space();
        if (!consume(']')) {
            while (true) {
                skip_space();
                Expected<JsonRef, Error> element = parse_value(depth + 1);
                if (!element) {
                    return make_unexpected(element.error());
                }
                collected.push_back(Child{0, 0, element.value()});
                skip_space();
                if (consume(',')) {
                    // A trailing comma is refused here rather than accepted: it is the single
                    // commonest thing a lenient parser takes and a strict one rejects.
                    continue;
                }
                if (consume(']')) {
                    break;
                }
                return fail(ErrorCode::InvalidArgument, "expected ',' or ']' in an array");
            }
        }
        return finish_container(JsonKind::Array, collected);
    }

    [[nodiscard]] Expected<JsonRef, Error> parse_object(u32 depth) noexcept {
        if (!consume('{')) {
            return fail(ErrorCode::InvalidArgument, "expected an object");
        }
        std::vector<Child> collected;
        skip_space();
        if (!consume('}')) {
            while (true) {
                skip_space();
                Child child;
                if (Status read = parse_string(child.key_offset, child.key_length); !read) {
                    return make_unexpected(read.error());
                }
                skip_space();
                if (!consume(':')) {
                    return fail(ErrorCode::InvalidArgument, "expected ':' after an object key");
                }
                skip_space();
                Expected<JsonRef, Error> value = parse_value(depth + 1);
                if (!value) {
                    return make_unexpected(value.error());
                }
                child.value = value.value();
                for (const Child& existing : collected) {
                    const std::string_view a =
                        document_.text_of(existing.key_offset, existing.key_length);
                    const std::string_view b =
                        document_.text_of(child.key_offset, child.key_length);
                    if (a == b) {
                        // Two parsers disagree about which wins, so a document with a duplicate key
                        // does not have one meaning.
                        return fail(ErrorCode::InvalidArgument, "a duplicate key in one object");
                    }
                }
                collected.push_back(child);
                skip_space();
                if (consume(',')) {
                    continue;
                }
                if (consume('}')) {
                    break;
                }
                return fail(ErrorCode::InvalidArgument, "expected ',' or '}' in an object");
            }
        }
        return finish_container(JsonKind::Object, collected);
    }

    /// Append a container's children contiguously and create its node.
    ///
    /// Contiguity is why children are collected locally first: a nested container appends its own
    /// children during its parse, so appending ours as we went would interleave the two.
    [[nodiscard]] Expected<JsonRef, Error> finish_container(
        JsonKind kind, const std::vector<Child>& collected) noexcept {
        Node node;
        node.kind = kind;
        node.first_child = static_cast<u32>(document_.children_.size());
        node.child_count = static_cast<u32>(collected.size());
        for (const Child& child : collected) {
            if (Status pushed = document_.children_.push_back(child); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
        return add(node);
    }

    [[nodiscard]] Expected<JsonRef, Error> parse_value(u32 depth) noexcept {
        if (depth > kMaxDepth) {
            return fail(ErrorCode::OutOfRange,
                        "a JSON document nested deeper than this build reads");
        }
        if (cursor_ >= text_.size()) {
            return fail(ErrorCode::InvalidArgument,
                        "a document that ends where a value was expected");
        }
        switch (text_[cursor_]) {
            case '{':
                return parse_object(depth);
            case '[':
                return parse_array(depth);
            case '"': {
                Node node;
                node.kind = JsonKind::String;
                if (Status read = parse_string(node.text_offset, node.text_length); !read) {
                    return make_unexpected(read.error());
                }
                return add(node);
            }
            case 't': {
                if (!literal("true")) {
                    return fail(ErrorCode::InvalidArgument, "expected 'true'");
                }
                Node node;
                node.kind = JsonKind::Bool;
                node.boolean = true;
                return add(node);
            }
            case 'f': {
                if (!literal("false")) {
                    return fail(ErrorCode::InvalidArgument, "expected 'false'");
                }
                Node node;
                node.kind = JsonKind::Bool;
                node.boolean = false;
                return add(node);
            }
            case 'n': {
                if (!literal("null")) {
                    return fail(ErrorCode::InvalidArgument, "expected 'null'");
                }
                return add(Node{});
            }
            default:
                return parse_number();
        }
    }

    JsonDocument& document_;
    std::string_view text_;
    usize cursor_ = 0;
    /// Reused across every string in the document, so escape decoding costs no allocation per
    /// string after the first long one.
    std::vector<char> scratch_;
};

// --- The document --------------------------------------------------------------------------------

Expected<JsonDocument, Error> JsonDocument::parse(std::string_view text) noexcept {
    JsonDocument document;
    Parser parser(document, text);
    if (Status parsed = parser.run(); !parsed) {
        return make_unexpected(parsed.error());
    }
    return document;
}

std::string_view JsonDocument::text_of(u32 offset, u32 length) const noexcept {
    if (length == 0) {
        return {};
    }
    return {text_.data() + offset, length};
}

JsonRef JsonDocument::root() const noexcept {
    // The last node, because nodes are created after their children. A container's index is
    // therefore always greater than its children's, and the root is the highest of all.
    return values_.empty() ? kNone : static_cast<JsonRef>(values_.size() - 1);
}

JsonKind JsonDocument::kind(JsonRef value) const noexcept {
    return value < values_.size() ? values_[value].kind : JsonKind::Null;
}

bool JsonDocument::is(JsonRef value, JsonKind wanted) const noexcept {
    return value < values_.size() && values_[value].kind == wanted;
}

usize JsonDocument::size(JsonRef value) const noexcept {
    if (value >= values_.size()) {
        return 0;
    }
    const Node& node = values_[value];
    return node.kind == JsonKind::Array || node.kind == JsonKind::Object ? node.child_count : 0;
}

JsonRef JsonDocument::at(JsonRef value, usize index) const noexcept {
    if (value >= values_.size()) {
        return kNone;
    }
    const Node& node = values_[value];
    if (node.kind != JsonKind::Array || index >= node.child_count) {
        return kNone;
    }
    return children_[node.first_child + index].value;
}

JsonRef JsonDocument::member(JsonRef value, std::string_view key) const noexcept {
    if (value >= values_.size()) {
        return kNone;
    }
    const Node& node = values_[value];
    if (node.kind != JsonKind::Object) {
        return kNone;
    }
    for (u32 index = 0; index < node.child_count; ++index) {
        const Child& child = children_[node.first_child + index];
        if (text_of(child.key_offset, child.key_length) == key) {
            return child.value;
        }
    }
    return kNone;
}

std::string_view JsonDocument::key_at(JsonRef value, usize index) const noexcept {
    if (value >= values_.size()) {
        return {};
    }
    const Node& node = values_[value];
    if (node.kind != JsonKind::Object || index >= node.child_count) {
        return {};
    }
    const Child& child = children_[node.first_child + index];
    return text_of(child.key_offset, child.key_length);
}

f64 JsonDocument::number_or(JsonRef value, f64 fallback) const noexcept {
    return is(value, JsonKind::Number) ? values_[value].number : fallback;
}

i64 JsonDocument::integer_or(JsonRef value, i64 fallback) const noexcept {
    return is(value, JsonKind::Number) ? static_cast<i64>(values_[value].number) : fallback;
}

bool JsonDocument::bool_or(JsonRef value, bool fallback) const noexcept {
    return is(value, JsonKind::Bool) ? values_[value].boolean : fallback;
}

std::string_view JsonDocument::string_or(JsonRef value, std::string_view fallback) const noexcept {
    if (!is(value, JsonKind::String)) {
        return fallback;
    }
    return text_of(values_[value].text_offset, values_[value].text_length);
}

}  // namespace cy::import
