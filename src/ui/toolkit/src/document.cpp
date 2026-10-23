// SPDX-License-Identifier: MIT
// The `.cyui` text form: parsing, writing, editing, and the cooked bundle. See document.h.

#include <cy/ui/toolkit/document.h>

#include <cy/core/assets/file.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace cy::ui {
namespace {

constexpr std::string_view kHeader = "cyui 1";
constexpr std::string_view kBundleHeader = "cyuicook 1\n";

[[nodiscard]] bool is_space(char byte) noexcept {
    return byte == ' ' || byte == '\t' || byte == '\r';
}

[[nodiscard]] bool is_ident(char byte) noexcept {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '-' || byte == '_';
}

/// Insert `value` at `index`, moving what is after it along.
template <typename T>
[[nodiscard]] Status insert_at(Array<T>& array, usize index, const T& value) noexcept {
    if (Status pushed = array.push_back(value); !pushed) {
        return pushed;
    }
    std::rotate(array.begin() + index, array.end() - 1, array.end());
    return ok();
}

/// A whole token that is a number: an optional sign, digits, an optional fraction.
[[nodiscard]] bool parse_number(std::string_view text, f32& out) noexcept {
    usize cursor = 0;
    f32 sign = 1.0F;
    if (cursor < text.size() && (text[cursor] == '-' || text[cursor] == '+')) {
        sign = text[cursor] == '-' ? -1.0F : 1.0F;
        ++cursor;
    }
    f32 value = 0.0F;
    bool digits = false;
    while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
        value = (value * 10.0F) + static_cast<f32>(text[cursor] - '0');
        digits = true;
        ++cursor;
    }
    if (cursor < text.size() && text[cursor] == '.') {
        ++cursor;
        f32 scale = 0.1F;
        while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
            value += static_cast<f32>(text[cursor] - '0') * scale;
            scale *= 0.1F;
            digits = true;
            ++cursor;
        }
    }
    if (!digits || cursor != text.size()) {
        return false;
    }
    out = value * sign;
    return true;
}

}  // namespace

bool DocumentNode::is_use() const noexcept {
    return kind.text() == "use";
}

Document::Document(Allocator& allocator) noexcept
    : nodes_(allocator),
      properties_(allocator),
      items_(allocator),
      parameters_(allocator),
      stylesheets_(allocator),
      diagnostics_(allocator),
      pool_(allocator),
      directive_lines_(allocator) {}

Span<const DocumentProperty> Document::properties(const DocumentNode& node) const noexcept {
    return properties_.span().subspan(node.first_property, node.property_count);
}

std::string_view Document::text(const DocumentValue& value) const noexcept {
    if (value.first + value.length > pool_.size()) {
        return {};
    }
    return {pool_.data() + value.first, value.length};
}

Span<const DocumentValue> Document::items(const DocumentValue& value) const noexcept {
    if (value.kind != DocumentValue::Kind::List ||
        value.first_item + value.item_count > items_.size()) {
        return {};
    }
    return items_.span().subspan(value.first_item, value.item_count);
}

const DocumentProperty* Document::find(u32 node, Name key) const noexcept {
    if (node >= nodes_.size()) {
        return nullptr;
    }
    for (const DocumentProperty& property : properties(nodes_[node])) {
        if (property.key == key) {
            return &property;
        }
    }
    return nullptr;
}

const DocumentValue* Document::parameter(Name name) const noexcept {
    for (const DocumentParameter& parameter : parameters_.span()) {
        if (parameter.name == name) {
            return &parameter.value;
        }
    }
    return nullptr;
}

Expected<u32, Error> Document::intern(std::string_view text) noexcept {
    const auto first = static_cast<u32>(pool_.size());
    if (Status kept = pool_.append(Span<const char>(text.data(), text.size())); !kept) {
        return make_unexpected(kept.error());
    }
    return first;
}

// --- Editing -------------------------------------------------------------------------------------

Expected<u32, Error> Document::add(u32 parent, std::string_view kind) noexcept {
    if (parent != DocumentNode::kNoParent && parent >= nodes_.size()) {
        return make_unexpected(Error{ErrorCode::NotFound, "document: no such parent", 0});
    }
    // After the parent's last descendant, which keeps the lines in tree order.
    usize at = nodes_.size();
    u32 depth = 0;
    if (parent != DocumentNode::kNoParent) {
        depth = nodes_[parent].depth + 1;
        at = parent + 1U;
        while (at < nodes_.size() && nodes_[at].depth >= depth) {
            ++at;
        }
    }
    DocumentNode node;
    node.kind = Name::intern(kind);
    node.parent = parent;
    node.depth = depth;
    node.first_property = at < nodes_.size() ? nodes_[at].first_property
                                             : static_cast<u32>(properties_.size());
    if (Status inserted = insert_at(nodes_, at, node); !inserted) {
        return make_unexpected(inserted.error());
    }
    for (usize index = at + 1; index < nodes_.size(); ++index) {
        if (nodes_[index].parent != DocumentNode::kNoParent && nodes_[index].parent >= at) {
            ++nodes_[index].parent;
        }
    }
    return static_cast<u32>(at);
}

Status Document::set_id(u32 node, std::string_view id) noexcept {
    if (node >= nodes_.size()) {
        return fail(ErrorCode::NotFound, "document: no such node");
    }
    nodes_[node].id = Name::intern(id);
    return ok();
}

Status Document::add_class(u32 node, std::string_view name) noexcept {
    if (node >= nodes_.size() || nodes_[node].class_count >= 4) {
        return fail(ErrorCode::InvalidArgument, "document: no such node, or four classes already");
    }
    nodes_[node].classes[nodes_[node].class_count++] = Name::intern(name);
    return ok();
}

Status Document::set_value(u32 node, std::string_view key, const DocumentValue& value) noexcept {
    if (node >= nodes_.size()) {
        return fail(ErrorCode::NotFound, "document: no such node");
    }
    const Name wanted = Name::intern(key);
    DocumentNode& target = nodes_[node];
    for (u32 index = 0; index < target.property_count; ++index) {
        DocumentProperty& property = properties_[target.first_property + index];
        if (property.key == wanted) {
            // IN ITS PLACE ON THE LINE: a changed property is a changed token, nothing else moves.
            property.value = value;
            return ok();
        }
    }
    DocumentProperty property;
    property.key = wanted;
    property.value = value;
    const u32 at = target.first_property + target.property_count;
    if (Status inserted = insert_at(properties_, at, property); !inserted) {
        return inserted;
    }
    ++nodes_[node].property_count;
    // Properties are stored in node order, so every node after this one starts one later.
    for (usize index = node + 1U; index < nodes_.size(); ++index) {
        ++nodes_[index].first_property;
    }
    return ok();
}

Status Document::set_number(u32 node, std::string_view key, f32 value) noexcept {
    char spelled[32] = {};
    const int length = std::snprintf(spelled, sizeof(spelled), "%g", static_cast<double>(value));
    Expected<u32, Error> first = intern(std::string_view(spelled, static_cast<usize>(length)));
    if (!first) {
        return make_unexpected(first.error());
    }
    DocumentValue number;
    number.kind = DocumentValue::Kind::Number;
    number.number = value;
    number.first = *first;
    number.length = static_cast<u32>(length);
    return set_value(node, key, number);
}

Status Document::set_string(u32 node, std::string_view key, std::string_view value) noexcept {
    Expected<u32, Error> first = intern(value);
    if (!first) {
        return make_unexpected(first.error());
    }
    DocumentValue string;
    string.kind = DocumentValue::Kind::String;
    string.first = *first;
    string.length = static_cast<u32>(value.size());
    return set_value(node, key, string);
}

Status Document::set_word(u32 node, std::string_view key, std::string_view value) noexcept {
    Expected<u32, Error> first = intern(value);
    if (!first) {
        return make_unexpected(first.error());
    }
    DocumentValue word;
    word.kind = DocumentValue::Kind::Word;
    word.first = *first;
    word.length = static_cast<u32>(value.size());
    return set_value(node, key, word);
}

// --- Parsing -------------------------------------------------------------------------------------

/// The parser is a friend of `Document` so its arrays stay private to everyone else.
class DocumentParser {
public:
    DocumentParser(std::string_view text, Document& document) noexcept
        : text_(text), document_(&document), stack_(document.nodes_.allocator()) {}

    [[nodiscard]] Status run() noexcept {
        usize cursor = 0;
        u32 line = 0;
        bool header = false;
        while (cursor <= text_.size()) {
            const usize end = std::min(text_.find('\n', cursor), text_.size());
            ++line;
            const std::string_view content = text_.substr(cursor, end - cursor);
            cursor = end + 1;
            if (!header) {
                header = true;
                if (content != kHeader) {
                    if (Status reported = report("a document starts `cyui 1`", line, 1); !reported) {
                        return reported;
                    }
                }
                continue;
            }
            if (end == text_.size() && content.empty()) {
                break;
            }
            if (Status parsed = parse_line(content, line); !parsed) {
                return parsed;
            }
        }
        return ok();
    }

private:
    [[nodiscard]] Status report(const char* message, u32 line, u32 column,
                                std::string_view detail = {}) noexcept {
        DocumentDiagnostic diagnostic;
        diagnostic.message = message;
        diagnostic.line = line;
        diagnostic.column = column;
        if (!detail.empty()) {
            diagnostic.detail = Name::intern(detail);
        }
        return document_->diagnostics_.push_back(diagnostic);
    }

    [[nodiscard]] Status parse_line(std::string_view content, u32 line) noexcept {
        usize spaces = 0;
        while (spaces < content.size() && content[spaces] == ' ') {
            ++spaces;
        }
        const std::string_view rest = content.substr(spaces);
        if (rest.empty() || rest.starts_with("//")) {
            return comment(rest.empty() ? rest : rest.substr(2), static_cast<u32>(spaces / 2), line);
        }
        if (spaces % 2 != 0) {
            if (Status reported = report("indentation is two spaces a level", line, 1); !reported) {
                return reported;
            }
        }
        const auto depth = static_cast<u32>(spaces / 2);
        usize word_end = 0;
        while (word_end < rest.size() && !is_space(rest[word_end])) {
            ++word_end;
        }
        const std::string_view word = rest.substr(0, word_end);
        if (word == "stylesheet" || word == "param") {
            return directive(word, rest.substr(word_end), depth, line,
                             static_cast<u32>(spaces + word_end + 1));
        }
        return element(word, rest.substr(word_end), depth, line,
                       static_cast<u32>(spaces + word_end + 1));
    }

    [[nodiscard]] Status comment(std::string_view text, u32 depth, u32 line) noexcept {
        Expected<u32, Error> first = document_->intern(text);
        if (!first) {
            return make_unexpected(first.error());
        }
        DocumentNode node;
        node.depth = depth;
        node.line = line;
        node.path.kind = DocumentValue::Kind::String;
        node.path.first = *first;
        node.path.length = static_cast<u32>(text.size());
        node.first_property = static_cast<u32>(document_->properties_.size());
        // A blank line is a comment of nothing at no depth; the writer gives back an empty line.
        node.path.item_count = text.empty() && depth == 0 ? 1U : 0U;
        return document_->nodes_.push_back(node);
    }

    [[nodiscard]] Status directive(std::string_view word, std::string_view rest, u32 depth,
                                   u32 line, u32 column) noexcept {
        if (depth != 0 || !document_->nodes_.empty()) {
            if (Status reported =
                    report("a directive comes before the first element, unindented", line, column);
                !reported) {
                return reported;
            }
        }
        usize cursor = 0;
        if (word == "stylesheet") {
            DocumentValue path;
            if (Status read = value(rest, cursor, line, column, path); !read) {
                return read;
            }
            const auto index = static_cast<u32>(document_->stylesheets_.size());
            if (Status pushed = document_->stylesheets_.push_back(path); !pushed) {
                return pushed;
            }
            return document_->directive_lines_.push_back(index);
        }
        skip_spaces(rest, cursor);
        const usize name_begin = cursor;
        while (cursor < rest.size() && is_ident(rest[cursor])) {
            ++cursor;
        }
        DocumentParameter parameter;
        parameter.name = Name::intern(rest.substr(name_begin, cursor - name_begin));
        if (Status read = value(rest, cursor, line, column, parameter.value); !read) {
            return read;
        }
        const auto index = static_cast<u32>(document_->parameters_.size());
        if (Status pushed = document_->parameters_.push_back(parameter); !pushed) {
            return pushed;
        }
        return document_->directive_lines_.push_back(index | 0x80000000U);
    }

    [[nodiscard]] Status element(std::string_view kind, std::string_view rest, u32 depth, u32 line,
                                 u32 column) noexcept {
        DocumentNode node;
        node.kind = Name::intern(kind);
        node.depth = depth;
        node.line = line;
        node.first_property = static_cast<u32>(document_->properties_.size());
        // THE PARENT IS THE LAST ELEMENT ONE LEVEL UP. Deeper than one past it is a diagnostic,
        // and the element is attached as deep as it can be.
        while (stack_.size() > depth) {
            stack_.pop_back();
        }
        if (stack_.size() < depth) {
            if (Status reported = report("indented more than one level past its parent", line, 1);
                !reported) {
                return reported;
            }
        }
        node.parent = stack_.empty() ? DocumentNode::kNoParent : stack_[stack_.size() - 1];
        node.depth = static_cast<u32>(stack_.size());
        usize cursor = 0;
        if (kind == "use") {
            if (Status read = value(rest, cursor, line, column, node.path); !read) {
                return read;
            }
            if (node.path.kind != DocumentValue::Kind::String) {
                if (Status reported = report("`use` takes a quoted path", line, column);
                    !reported) {
                    return reported;
                }
            }
        }
        while (true) {
            skip_spaces(rest, cursor);
            if (cursor >= rest.size()) {
                break;
            }
            const auto at = static_cast<u32>(column + cursor);
            if (Status token = parse_token(rest, cursor, line, at, node); !token) {
                return token;
            }
        }
        const auto index = static_cast<u32>(document_->nodes_.size());
        if (Status pushed = document_->nodes_.push_back(node); !pushed) {
            return pushed;
        }
        return stack_.push_back(index);
    }

    [[nodiscard]] Status parse_token(std::string_view rest, usize& cursor, u32 line, u32 column,
                                     DocumentNode& node) noexcept {
        const char lead = rest[cursor];
        if (lead == '#' || lead == '.') {
            ++cursor;
            const usize begin = cursor;
            while (cursor < rest.size() && is_ident(rest[cursor])) {
                ++cursor;
            }
            const std::string_view name = rest.substr(begin, cursor - begin);
            if (name.empty()) {
                ++cursor;
                return report("an id or a class needs a name", line, column);
            }
            if (lead == '#') {
                node.id = Name::intern(name);
            } else if (node.class_count < 4) {
                node.classes[node.class_count++] = Name::intern(name);
            } else {
                return report("at most four classes an element", line, column, name);
            }
            return ok();
        }
        const usize begin = cursor;
        while (cursor < rest.size() && is_ident(rest[cursor])) {
            ++cursor;
        }
        const std::string_view key = rest.substr(begin, cursor - begin);
        if (key.empty() || cursor >= rest.size() || rest[cursor] != '=') {
            while (cursor < rest.size() && !is_space(rest[cursor])) {
                ++cursor;
            }
            return report("a token is `#id`, `.class` or `key=value`", line, column,
                          rest.substr(begin, cursor - begin));
        }
        ++cursor;
        DocumentProperty property;
        property.key = Name::intern(key);
        property.column = column;
        if (Status read = value(rest, cursor, line, column, property.value); !read) {
            return read;
        }
        if (Status pushed = document_->properties_.push_back(property); !pushed) {
            return pushed;
        }
        ++node.property_count;
        return ok();
    }

    static void skip_spaces(std::string_view text, usize& cursor) noexcept {
        while (cursor < text.size() && is_space(text[cursor])) {
            ++cursor;
        }
    }

    [[nodiscard]] Status value(std::string_view text, usize& cursor, u32 line, u32 column,
                               DocumentValue& out) noexcept {
        skip_spaces(text, cursor);
        if (cursor >= text.size()) {
            return report("a value is missing", line, column);
        }
        const char lead = text[cursor];
        if (lead == '"') {
            return string(text, cursor, line, column, out);
        }
        if (lead == '[') {
            return list(text, cursor, line, column, out);
        }
        const usize begin = cursor;
        while (cursor < text.size() && !is_space(text[cursor]) && text[cursor] != ',' &&
               text[cursor] != ']') {
            ++cursor;
        }
        std::string_view token = text.substr(begin, cursor - begin);
        out = DocumentValue{};
        if (lead == '$') {
            out.kind = DocumentValue::Kind::Parameter;
            token = token.substr(1);
        } else if (parse_number(token, out.number)) {
            out.kind = DocumentValue::Kind::Number;
        } else {
            out.kind = DocumentValue::Kind::Word;
        }
        Expected<u32, Error> first = document_->intern(token);
        if (!first) {
            return make_unexpected(first.error());
        }
        out.first = *first;
        out.length = static_cast<u32>(token.size());
        return ok();
    }

    [[nodiscard]] Status string(std::string_view text, usize& cursor, u32 line, u32 column,
                                DocumentValue& out) noexcept {
        Array<char> unescaped(document_->pool_.allocator());
        ++cursor;
        bool closed = false;
        while (cursor < text.size()) {
            char byte = text[cursor++];
            if (byte == '"') {
                closed = true;
                break;
            }
            if (byte == '\\' && cursor < text.size()) {
                const char escaped = text[cursor++];
                byte = escaped == 'n' ? '\n' : escaped;
            }
            if (Status kept = unescaped.push_back(byte); !kept) {
                return kept;
            }
        }
        if (!closed) {
            if (Status reported = report("a string is not closed", line, column); !reported) {
                return reported;
            }
        }
        Expected<u32, Error> first =
            document_->intern(std::string_view(unescaped.data(), unescaped.size()));
        if (!first) {
            return make_unexpected(first.error());
        }
        out = DocumentValue{};
        out.kind = DocumentValue::Kind::String;
        out.first = *first;
        out.length = static_cast<u32>(unescaped.size());
        return ok();
    }

    [[nodiscard]] Status list(std::string_view text, usize& cursor, u32 line, u32 column,
                              DocumentValue& out) noexcept {
        ++cursor;
        Array<DocumentValue> items(document_->items_.allocator());
        while (true) {
            skip_spaces(text, cursor);
            if (cursor >= text.size()) {
                if (Status reported = report("a list is not closed", line, column); !reported) {
                    return reported;
                }
                break;
            }
            if (text[cursor] == ']') {
                ++cursor;
                break;
            }
            DocumentValue item;
            if (Status read = value(text, cursor, line, column, item); !read) {
                return read;
            }
            if (Status pushed = items.push_back(item); !pushed) {
                return pushed;
            }
            skip_spaces(text, cursor);
            if (cursor < text.size() && text[cursor] == ',') {
                ++cursor;
            }
        }
        // Items are stored contiguously, after any a nested list stored while they were read.
        out = DocumentValue{};
        out.kind = DocumentValue::Kind::List;
        out.first_item = static_cast<u32>(document_->items_.size());
        out.item_count = static_cast<u32>(items.size());
        return document_->items_.append(items.span());
    }

    std::string_view text_;
    Document* document_;
    /// The last element at each depth: the parents of the next line.
    Array<u32> stack_;
};

Status parse_document(std::string_view text, Document& out) noexcept {
    DocumentParser parser(text, out);
    return parser.run();
}

// --- Writing -------------------------------------------------------------------------------------

namespace {

[[nodiscard]] Status put(Array<char>& out, std::string_view text) noexcept {
    return out.append(Span<const char>(text.data(), text.size()));
}

[[nodiscard]] Status write_value(const Document& document, const DocumentValue& value,
                                 Array<char>& out) noexcept {
    switch (value.kind) {
        case DocumentValue::Kind::Number:
        case DocumentValue::Kind::Word:
            return put(out, document.text(value));
        case DocumentValue::Kind::Parameter:
            if (Status dollar = out.push_back('$'); !dollar) {
                return dollar;
            }
            return put(out, document.text(value));
        case DocumentValue::Kind::String: {
            if (Status quote = out.push_back('"'); !quote) {
                return quote;
            }
            for (const char byte : document.text(value)) {
                if (byte == '"' || byte == '\\') {
                    if (Status escape = out.push_back('\\'); !escape) {
                        return escape;
                    }
                }
                if (Status kept = (byte == '\n') ? put(out, "\\n") : out.push_back(byte); !kept) {
                    return kept;
                }
            }
            return out.push_back('"');
        }
        case DocumentValue::Kind::List: {
            if (Status open = out.push_back('['); !open) {
                return open;
            }
            const Span<const DocumentValue> items = document.items(value);
            for (usize index = 0; index < items.size(); ++index) {
                if (index > 0) {
                    if (Status comma = put(out, ", "); !comma) {
                        return comma;
                    }
                }
                if (Status item = write_value(document, items[index], out); !item) {
                    return item;
                }
            }
            return out.push_back(']');
        }
    }
    return ok();
}

[[nodiscard]] Status write_node(const Document& document, const DocumentNode& node,
                                Array<char>& out) noexcept {
    for (u32 level = 0; level < node.depth; ++level) {
        if (Status indent = put(out, "  "); !indent) {
            return indent;
        }
    }
    if (node.is_comment()) {
        if (node.path.item_count == 1U) {
            return out.push_back('\n');
        }
        if (Status slashes = put(out, "//"); !slashes) {
            return slashes;
        }
        if (Status body = put(out, document.text(node.path)); !body) {
            return body;
        }
        return out.push_back('\n');
    }
    if (Status kind = put(out, node.kind.text()); !kind) {
        return kind;
    }
    if (node.is_use()) {
        if (Status space = out.push_back(' '); !space) {
            return space;
        }
        if (Status path = write_value(document, node.path, out); !path) {
            return path;
        }
    }
    if (!node.id.is_empty()) {
        if (Status id = put(out, " #"); !id) {
            return id;
        }
        if (Status name = put(out, node.id.text()); !name) {
            return name;
        }
    }
    for (u32 index = 0; index < node.class_count; ++index) {
        if (Status dot = put(out, " ."); !dot) {
            return dot;
        }
        if (Status name = put(out, node.classes[index].text()); !name) {
            return name;
        }
    }
    for (const DocumentProperty& property : document.properties(node)) {
        if (Status space = out.push_back(' '); !space) {
            return space;
        }
        if (Status key = put(out, property.key.text()); !key) {
            return key;
        }
        if (Status equals = out.push_back('='); !equals) {
            return equals;
        }
        if (Status value = write_value(document, property.value, out); !value) {
            return value;
        }
    }
    return out.push_back('\n');
}

}  // namespace

Status write_document(const Document& document, Array<char>& out) noexcept {
    out.clear();
    if (Status header = put(out, kHeader); !header) {
        return header;
    }
    if (Status newline = out.push_back('\n'); !newline) {
        return newline;
    }
    for (const u32 directive : document.directives()) {
        const bool parameter = (directive & 0x80000000U) != 0U;
        const u32 index = directive & 0x7FFFFFFFU;
        if (parameter) {
            const DocumentParameter& entry = document.parameters()[index];
            if (Status word = put(out, "param "); !word) {
                return word;
            }
            if (Status name = put(out, entry.name.text()); !name) {
                return name;
            }
            if (Status space = out.push_back(' '); !space) {
                return space;
            }
            if (Status value = write_value(document, entry.value, out); !value) {
                return value;
            }
        } else {
            if (Status word = put(out, "stylesheet "); !word) {
                return word;
            }
            if (Status value = write_value(document, document.stylesheets()[index], out); !value) {
                return value;
            }
        }
        if (Status newline = out.push_back('\n'); !newline) {
            return newline;
        }
    }
    for (const DocumentNode& node : document.nodes()) {
        if (Status written = write_node(document, node, out); !written) {
            return written;
        }
    }
    return ok();
}

Status document_dependencies(const Document& document, Array<std::string_view>& paths) noexcept {
    for (const DocumentValue& sheet : document.stylesheets()) {
        if (Status pushed = paths.push_back(document.text(sheet)); !pushed) {
            return pushed;
        }
    }
    for (const DocumentNode& node : document.nodes()) {
        if (node.is_use()) {
            if (Status pushed = paths.push_back(document.text(node.path)); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

// --- Resolvers and bundles -----------------------------------------------------------------------

FileDocumentResolver::FileDocumentResolver(Allocator& allocator, std::string_view root) noexcept
    : root_(allocator) {
    (void)root_.append(Span<const char>(root.data(), root.size()));
}

void FileDocumentResolver::set_root(std::string_view root) noexcept {
    root_.clear();
    (void)root_.append(Span<const char>(root.data(), root.size()));
}

Status FileDocumentResolver::full_path(std::string_view path, Array<char>& full) const noexcept {
    full.clear();
    if (Status built = full.append(root_.span()); !built) {
        return built;
    }
    if (!full.empty() && full[full.size() - 1] != '/') {
        if (Status slash = full.push_back('/'); !slash) {
            return slash;
        }
    }
    if (Status built = full.append(Span<const char>(path.data(), path.size())); !built) {
        return built;
    }
    return full.push_back('\0');
}

Status FileDocumentResolver::read(std::string_view path, Array<char>& out) noexcept {
    Array<char> full(root_.allocator());
    if (Status built = full_path(path, full); !built) {
        return built;
    }
    Expected<assets::File, Error> file = assets::File::open(full.data(), assets::FileMode::Read);
    if (!file) {
        return make_unexpected(file.error());
    }
    Expected<u64, Error> size = file->size();
    if (!size) {
        return make_unexpected(size.error());
    }
    const usize start = out.size();
    if (Status grown = out.resize(start + static_cast<usize>(*size)); !grown) {
        return grown;
    }
    Expected<usize, Error> read = file->read(out.data() + start, static_cast<usize>(*size));
    if (!read) {
        return make_unexpected(read.error());
    }
    return out.resize(start + *read);
}

Status parse_bundle(std::string_view text, Array<BundleEntry>& out) noexcept {
    out.clear();
    if (!text.starts_with(kBundleHeader)) {
        return fail(ErrorCode::InvalidArgument, "ui bundle: it starts `cyuicook 1`");
    }
    usize cursor = kBundleHeader.size();
    while (cursor < text.size()) {
        const usize end = text.find('\n', cursor);
        if (end == std::string_view::npos) {
            return fail(ErrorCode::InvalidArgument, "ui bundle: an entry's line is not ended");
        }
        const std::string_view header = text.substr(cursor, end - cursor);
        const usize first_space = header.find(' ');
        const usize last_space = header.rfind(' ');
        if (first_space == std::string_view::npos || last_space == first_space) {
            return fail(ErrorCode::InvalidArgument, "ui bundle: an entry is `kind path bytes`");
        }
        usize bytes = 0;
        for (const char digit : header.substr(last_space + 1)) {
            if (digit < '0' || digit > '9') {
                return fail(ErrorCode::InvalidArgument, "ui bundle: an entry's size is a number");
            }
            bytes = (bytes * 10U) + static_cast<usize>(digit - '0');
        }
        const usize body = end + 1;
        if (body + bytes > text.size()) {
            return fail(ErrorCode::InvalidArgument, "ui bundle: an entry runs past the end");
        }
        BundleEntry entry;
        entry.kind = header.substr(0, first_space);
        entry.path = header.substr(first_space + 1, last_space - first_space - 1);
        entry.bytes = text.substr(body, bytes);
        if (Status pushed = out.push_back(entry); !pushed) {
            return pushed;
        }
        cursor = body + bytes;
        if (cursor < text.size() && text[cursor] == '\n') {
            ++cursor;
        }
    }
    return ok();
}

Status append_bundle_entry(Array<char>& bundle, std::string_view kind, std::string_view path,
                           std::string_view bytes) noexcept {
    if (bundle.empty()) {
        if (Status header = put(bundle, kBundleHeader); !header) {
            return header;
        }
    }
    char size[24] = {};
    const int length = std::snprintf(size, sizeof(size), "%zu", bytes.size());
    for (const std::string_view part :
         {kind, std::string_view(" "), path, std::string_view(" "),
          std::string_view(size, static_cast<usize>(length)), std::string_view("\n"), bytes,
          std::string_view("\n")}) {
        if (Status appended = put(bundle, part); !appended) {
            return appended;
        }
    }
    return ok();
}

BundleResolver::BundleResolver(Allocator& allocator, std::string_view bundle) noexcept
    : entries_(allocator), status_(parse_bundle(bundle, entries_)) {}

std::string_view BundleResolver::main_document() const noexcept {
    for (const BundleEntry& entry : entries_.span()) {
        if (entry.kind == "document") {
            return entry.bytes;
        }
    }
    return {};
}

Status BundleResolver::read(std::string_view path, Array<char>& out) noexcept {
    for (const BundleEntry& entry : entries_.span()) {
        if (entry.path == path) {
            return out.append(Span<const char>(entry.bytes.data(), entry.bytes.size()));
        }
    }
    return fail(ErrorCode::NotFound, "ui bundle: no such path in it");
}

}  // namespace cy::ui
