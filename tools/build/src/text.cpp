#include "text.h"

#include <algorithm>

namespace cy::build::text {
namespace {

[[nodiscard]] Error invalid(const char* message) noexcept {
    return Error{ErrorCode::InvalidArgument, message, 0};
}

/// Read one quoted word, starting at the opening quote. Answers false on an unterminated quote.
[[nodiscard]] bool take_quoted(std::string_view line, usize& cursor, std::string& out) {
    ++cursor;  // the opening quote
    while (cursor < line.size()) {
        const char character = line[cursor];
        if (character == '\\' && cursor + 1 < line.size()) {
            out.push_back(line[cursor + 1]);
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
        if (character == '"' || character == '\\') {
            out.push_back('\\');
        }
        out.push_back(character);
    }
    out.push_back('"');
    return out;
}

}  // namespace cy::build::text
