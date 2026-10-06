#pragma once
// Helpers shared by the deterministic math suites: a seeded generator for random sweeps, and the
// parsing of the committed vector and oracle files.

#include <cy/core/base/types.h>
#include <cy/core/detmath/wide.h>

#include <string>
#include <string_view>
#include <vector>

namespace cy::detmath_test {

/// SplitMix64, for the tests' own sweeps. Seeded per case, so a failure reproduces.
class Rng {
public:
    explicit Rng(u64 seed) noexcept : state_(seed) {}

    u64 next() noexcept {
        state_ += 0x9E3779B97F4A7C15ULL;
        u64 z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    /// A raw value spread over every binade.
    i64 scaled() noexcept {
        const auto value = static_cast<i64>(next());
        const auto shift = static_cast<int>(next() & 63U);
        return value >> shift;
    }

private:
    u64 state_;
};

/// One whitespace-separated line, split.
inline std::vector<std::string_view> split(std::string_view line) {
    std::vector<std::string_view> fields;
    usize start = 0;
    while (start < line.size()) {
        while (start < line.size() &&
               (line[start] == ' ' || line[start] == '\t' || line[start] == '\r')) {
            ++start;
        }
        usize end = start;
        while (end < line.size() && line[end] != ' ' && line[end] != '\t' && line[end] != '\r') {
            ++end;
        }
        if (end > start) {
            fields.push_back(line.substr(start, end - start));
        }
        start = end;
    }
    return fields;
}

/// Unsigned hexadecimal, up to 32 digits, into 128 bits. False on anything else.
inline bool parse_hex(std::string_view text, detmath::U128& out) noexcept {
    if (text.empty() || text.size() > 32) {
        return false;
    }
    detmath::U128 value{};
    for (const char c : text) {
        u64 digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<u64>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<u64>(c - 'a' + 10);
        } else {
            return false;
        }
        value = detmath::wide::shl(value, 4);
        value.lo |= digit;
    }
    out = value;
    return true;
}

/// Signed hexadecimal (a leading '-' for negative) into a two's-complement 128-bit value.
inline bool parse_signed_hex(std::string_view text, detmath::U128& out) noexcept {
    const bool negative = !text.empty() && text[0] == '-';
    if (!parse_hex(negative ? text.substr(1) : text, out)) {
        return false;
    }
    if (negative) {
        out = detmath::wide::negate(out);
    }
    return true;
}

/// The lines of `text` that are not comments or blank.
inline std::vector<std::string_view> data_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    usize start = 0;
    while (start < text.size()) {
        usize end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line[0] != '#') {
            lines.push_back(line);
        }
        start = end + 1;
    }
    return lines;
}

}  // namespace cy::detmath_test
