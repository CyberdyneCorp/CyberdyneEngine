#include "text.h"

#include <algorithm>

namespace cy::build::text {
namespace {

[[nodiscard]] Error invalid(const char* message) noexcept {
    return Error{ErrorCode::InvalidArgument, message, 0};
}

/// Read one quoted word, starting at the opening quote. Answers false on an unterminated quote.
///
/// Backslash escapes: `\"` and `\\` are literal quotes and backslashes; `\n`, `\r` and `\t` are
/// their respective control characters. The escape set is symmetric with `quote()` below — a
/// value that survives the round-trip is one that carries newlines and quotes inside a single
/// physical line, which is what the toolchain fingerprint's multi-line description needs.
[[nodiscard]] bool take_quoted(std::string_view line, usize& cursor, std::string& out) {
    ++cursor;  // the opening quote
    while (cursor < line.size()) {
        const char character = line[cursor];
        if (character == '\\' && cursor + 1 < line.size()) {
            const char escaped = line[cursor + 1];
            switch (escaped) {
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                default:
                    out.push_back(escaped);
                    break;
            }
            cursor += 2;
            continue;
        }
        if (character == '"') {
            ++cursor;
            return true;
        }
        out.push_back(character);
        ++cursor;
    }
    return false;
}

[[nodiscard]] Status split(std::string_view line, Line& out) {
    usize cursor = 0;
    while (cursor < line.size() && line[cursor] == ' ') {
        ++cursor;
    }
    out.depth = static_cast<u32>(cursor / 2);

    while (cursor < line.size()) {
        if (line[cursor] == ' ') {
            ++cursor;
            continue;
        }
        std::string word;
        if (line[cursor] == '"') {
            if (!take_quoted(line, cursor, word)) {
                return make_unexpected(invalid("unterminated quoted word"));
            }
        } else {
            while (cursor < line.size() && line[cursor] != ' ') {
                word.push_back(line[cursor]);
                ++cursor;
            }
        }
        out.words.push_back(std::move(word));
    }
    return ok();
}

}  // namespace

Expected<std::vector<Line>, Error> read(std::string_view document) {
    std::vector<Line> lines;
    usize start = 0;
    u32 number = 0;
    while (start <= document.size()) {
        const usize newline = std::min(document.find('\n', start), document.size());
        std::string_view raw = document.substr(start, newline - start);
        start = newline + 1;
        ++number;

        if (!raw.empty() && raw.back() == '\r') {
            raw.remove_suffix(1);
        }
        const usize first = raw.find_first_not_of(" \t");
        if (first == std::string_view::npos || raw[first] == '#') {
            continue;
        }

        Line line;
        line.number = number;
        if (Status split_line = split(raw, line); !split_line) {
            return make_unexpected(split_line.error());
        }
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
    }
    return lines;
}

std::string quote(std::string_view word) {
    std::string out;
    out.reserve(word.size() + 2);
    out.push_back('"');
    for (const char character : word) {
        // A quoted word cannot span physical lines in this format — the reader splits on newlines
        // before parsing — so newlines and other whitespace controls are escaped rather than
        // embedded verbatim. The toolchain fingerprint's multi-line description is the case that
        // motivated this: its `compiler MSVC 19.44...\nflags c++20...` value used to serialise as
        // an unterminated-on-line-one quoted word and fail every round-trip.
        switch (character) {
            case '"':
                out.push_back('\\');
                out.push_back('"');
                continue;
            case '\\':
                out.push_back('\\');
                out.push_back('\\');
                continue;
            case '\n':
                out.push_back('\\');
                out.push_back('n');
                continue;
            case '\r':
                out.push_back('\\');
                out.push_back('r');
                continue;
            case '\t':
                out.push_back('\\');
                out.push_back('t');
                continue;
            default:
                out.push_back(character);
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace cy::build::text
