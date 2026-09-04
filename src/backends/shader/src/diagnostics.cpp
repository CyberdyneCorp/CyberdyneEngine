// The diagnostic log and the compiler-output parser. Task 3.7.
//
// The parser accepts the two shapes every toolchain the engine will ever call emits:
//
//   file(line): severity code: message        Slang, DXC, MSVC, glslang
//   file:line:column: severity: message       Clang, GCC
//
// A line matching neither is kept as an unlocated note rather than discarded, because the second
// half of a two-part diagnostic ("see declaration of 'x'") carries the information the first half
// is missing.

#include <cy/backends/shader/diagnostics.h>

#include <cstring>

namespace cy::shader {
namespace {

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    usize begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    usize end = text.size();
    while (end > begin &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

[[nodiscard]] bool parse_u32(std::string_view text, u32& out) noexcept {
    if (text.empty()) {
        return false;
    }
    u64 value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = (value * 10) + static_cast<u64>(c - '0');
        if (value > 0xFFFF'FFFFULL) {
            return false;
        }
    }
    out = static_cast<u32>(value);
    return true;
}

/// The severity keyword at `position`, or none. A keyword only counts when it stands alone: the
/// `error` inside `errors.slang` is a path, not a severity.
[[nodiscard]] bool severity_at(std::string_view line, usize position, Severity& out,
                               usize& length) noexcept {
    struct Keyword {
        std::string_view text;
        Severity severity;
    };
    static constexpr Keyword kKeywords[] = {
        {"error", Severity::Error},
        {"warning", Severity::Warning},
        {"note", Severity::Note},
        {"fatal error", Severity::Error},
    };
    for (const Keyword& keyword : kKeywords) {
        if (line.compare(position, keyword.text.size(), keyword.text) != 0) {
            continue;
        }
        const usize after = position + keyword.text.size();
        const bool starts_clean = position == 0 || line[position - 1] == ' ' ||
                                  line[position - 1] == ':' || line[position - 1] == '\t';
        const bool ends_clean =
            after >= line.size() || line[after] == ':' || line[after] == ' ' || line[after] == '[';
        if (starts_clean && ends_clean) {
            out = keyword.severity;
            length = keyword.text.size();
            return true;
        }
    }
    return false;
}

/// `file(line)` or `file(line,column)`. Returns false when the prefix is not that shape.
[[nodiscard]] bool parse_parenthesised(std::string_view prefix, std::string_view& file, u32& line,
                                       u32& column) noexcept {
    if (prefix.empty() || prefix.back() != ')') {
        return false;
    }
    const usize open = prefix.rfind('(');
    if (open == std::string_view::npos) {
        return false;
    }
    const std::string_view inside = prefix.substr(open + 1, prefix.size() - open - 2);
    const usize comma = inside.find(',');
    if (comma == std::string_view::npos) {
        if (!parse_u32(inside, line)) {
            return false;
        }
    } else {
        if (!parse_u32(inside.substr(0, comma), line) ||
            !parse_u32(inside.substr(comma + 1), column)) {
            return false;
        }
    }
    file = prefix.substr(0, open);
    return true;
}

/// `file:line:column` or `file:line`, taken from the right so a path containing a colon still
/// resolves. Returns false when the trailing groups are not numbers.
[[nodiscard]] bool parse_colon_separated(std::string_view prefix, std::string_view& file, u32& line,
                                         u32& column) noexcept {
    const usize last = prefix.rfind(':');
    if (last == std::string_view::npos || last == 0) {
        return false;
    }
    u32 first_number = 0;
    if (!parse_u32(prefix.substr(last + 1), first_number)) {
        return false;
    }
    const usize previous = prefix.rfind(':', last - 1);
    u32 second_number = 0;
    if (previous != std::string_view::npos && previous != 0 &&
        parse_u32(prefix.substr(previous + 1, last - previous - 1), second_number)) {
        file = prefix.substr(0, previous);
        line = second_number;
        column = first_number;
        return true;
    }
    file = prefix.substr(0, last);
    line = first_number;
    return true;
}

/// One line of compiler output, taken apart. A line the grammar does not recognise comes back as an
/// unlocated note rather than as nothing: the second half of a two-part diagnostic ("see
/// declaration of 'x'") carries what the first half is missing.
struct ParsedLine {
    Severity severity = Severity::Note;
    std::string_view file;
    std::string_view message;
    std::string_view code;
    u32 line = 0;
    u32 column = 0;
};

[[nodiscard]] ParsedLine parse_diagnostic_line(std::string_view line) noexcept {
    ParsedLine parsed;

    usize keyword = std::string_view::npos;
    usize keyword_length = 0;
    for (usize position = 0; position < line.size(); ++position) {
        if (severity_at(line, position, parsed.severity, keyword_length)) {
            keyword = position;
            break;
        }
    }
    if (keyword == std::string_view::npos) {
        parsed.severity = Severity::Note;
        parsed.message = line;
        return parsed;
    }

    // The prefix is the location, with its trailing ": " removed.
    std::string_view prefix = trim(line.substr(0, keyword));
    while (!prefix.empty() && (prefix.back() == ':' || prefix.back() == ' ')) {
        prefix.remove_suffix(1);
    }

    // After the keyword: an optional diagnostic number, then ':', then the message.
    std::string_view rest = trim(line.substr(keyword + keyword_length));
    const usize colon = rest.find(':');
    if (colon != std::string_view::npos) {
        const std::string_view between = trim(rest.substr(0, colon));
        if (!between.empty()) {
            parsed.code = between;
        }
        rest = trim(rest.substr(colon + 1));
    }
    parsed.message = rest;

    if (!parse_parenthesised(prefix, parsed.file, parsed.line, parsed.column) &&
        !parse_colon_separated(prefix, parsed.file, parsed.line, parsed.column)) {
        parsed.file = prefix;
        parsed.line = 0;
        parsed.column = 0;
    }
    return parsed;
}

}  // namespace

const char* severity_name(Severity severity) noexcept {
    switch (severity) {
        case Severity::Note:
            return "note";
        case Severity::Warning:
            return "warning";
        case Severity::Error:
            return "error";
    }
    return "unknown";
}

DiagnosticLog::DiagnosticLog(Allocator& allocator) noexcept
    : text_(allocator), records_(allocator) {}

Expected<u32, Error> DiagnosticLog::intern(std::string_view text) noexcept {
    if (text_.empty()) {
        // Offset zero is the empty string, so a zeroed record reads as "no file, no code".
        if (Status pushed = text_.push_back('\0'); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    if (text.empty()) {
        return u32{0};
    }
    const u32 offset = static_cast<u32>(text_.size());
    if (Status reserved = text_.reserve(text_.size() + text.size() + 1); !reserved) {
        return make_unexpected(reserved.error());
    }
    if (Status appended = text_.append(Span<const char>(text.data(), text.size())); !appended) {
        return make_unexpected(appended.error());
    }
    if (Status terminated = text_.push_back('\0'); !terminated) {
        return make_unexpected(terminated.error());
    }
    return offset;
}

Status DiagnosticLog::add(Severity severity, const SourceLocation& location,
                          std::string_view message, std::string_view code) noexcept {
    const std::string_view file = location.file != nullptr ? location.file : "";
    return add_parts(severity, file, location.line, location.column, message, code);
}

Status DiagnosticLog::add_parts(Severity severity, std::string_view file_text, u32 line_number,
                                u32 column, std::string_view message,
                                std::string_view code) noexcept {
    Expected<u32, Error> file = intern(file_text);
    if (!file) {
        return make_unexpected(file.error());
    }
    Expected<u32, Error> text = intern(message);
    if (!text) {
        return make_unexpected(text.error());
    }
    Expected<u32, Error> number = intern(code);
    if (!number) {
        return make_unexpected(number.error());
    }

    Record record;
    record.severity = severity;
    record.file = file.value();
    record.message = text.value();
    record.code = number.value();
    record.line = line_number;
    record.column = column;
    if (Status pushed = records_.push_back(record); !pushed) {
        return pushed;
    }

    if (severity == Severity::Error) {
        ++error_count_;
    } else if (severity == Severity::Warning) {
        ++warning_count_;
    }
    return ok();
}

Diagnostic DiagnosticLog::at(usize index) const noexcept {
    CY_ASSERT_MSG(index < records_.size(), "DiagnosticLog::at() past the end");
    const Record& record = records_[index];
    Diagnostic diagnostic;
    diagnostic.severity = record.severity;
    diagnostic.location.file = at_offset(record.file);
    diagnostic.location.line = record.line;
    diagnostic.location.column = record.column;
    diagnostic.message = at_offset(record.message);
    diagnostic.code = at_offset(record.code);
    return diagnostic;
}

Expected<bool, Error> DiagnosticLog::apply_location_continuation(std::string_view line) noexcept {
    // Slang's current diagnostic format puts the message on one line and the position on the next,
    // Rust-style. Attaching it to the record just emitted is what keeps "the error carries the
    // file, line and column" true against the toolchain as it actually is rather than as an older
    // version of it was.
    if (!line.starts_with("-->") || records_.empty()) {
        return false;
    }
    std::string_view file;
    u32 continuation_line = 0;
    u32 continuation_column = 0;
    if (!parse_colon_separated(trim(line.substr(3)), file, continuation_line,
                               continuation_column)) {
        return false;
    }
    Expected<u32, Error> offset = intern(file);
    if (!offset) {
        return make_unexpected(offset.error());
    }
    Record& record = records_.back();
    record.file = offset.value();
    record.line = continuation_line;
    record.column = continuation_column;
    return true;
}

Status DiagnosticLog::parse_compiler_output(std::string_view text) noexcept {
    usize begin = 0;
    while (begin <= text.size()) {
        usize end = text.find('\n', begin);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view line = trim(text.substr(begin, end - begin));
        begin = end + 1;
        if (line.empty()) {
            continue;
        }

        Expected<bool, Error> attached = apply_location_continuation(line);
        if (!attached) {
            return make_unexpected(attached.error());
        }
        if (attached.value()) {
            continue;
        }

        // `file`, `message` and `code` are views into the caller's buffer, never into this log's
        // arena, so interning them cannot read storage the growth has already freed.
        const ParsedLine parsed = parse_diagnostic_line(line);
        if (Status added = add_parts(parsed.severity, parsed.file, parsed.line, parsed.column,
                                     parsed.message, parsed.code);
            !added) {
            return added;
        }
    }
    return ok();
}

Status DiagnosticLog::append(const DiagnosticLog& other) noexcept {
    for (usize index = 0; index < other.size(); ++index) {
        const Diagnostic entry = other.at(index);
        if (Status added = add(entry.severity, entry.location, entry.message, entry.code); !added) {
            return added;
        }
    }
    return ok();
}

void DiagnosticLog::clear() noexcept {
    text_.clear();
    records_.clear();
    error_count_ = 0;
    warning_count_ = 0;
}

}  // namespace cy::shader
