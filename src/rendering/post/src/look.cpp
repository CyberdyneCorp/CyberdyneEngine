// SPDX-License-Identifier: MIT
#include <cy/rendering/post/look.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numbers>

namespace cy::rendering {
namespace {

using ScalarField = f32* (*)(Look&) noexcept;
using VectorField = Vec3* (*)(Look&) noexcept;

struct ScalarKey {
    std::string_view key;
    ScalarField field;
};

struct VectorKey {
    std::string_view key;
    VectorField field;
};

// The tables ARE the format: a key is legal exactly when it is in one of them.
constexpr ScalarKey kScalars[] = {
    {"temperature", [](Look& look) noexcept { return &look.settings.temperature; }},
    {"tint", [](Look& look) noexcept { return &look.settings.tint; }},
    {"contrast", [](Look& look) noexcept { return &look.settings.contrast; }},
    {"saturation", [](Look& look) noexcept { return &look.settings.saturation; }},
    {"hue-shift", [](Look& look) noexcept { return &look.settings.hue_shift; }},
    {"shadow-boundary", [](Look& look) noexcept { return &look.settings.shadow_boundary; }},
    {"highlight-boundary", [](Look& look) noexcept { return &look.settings.highlight_boundary; }},
};

constexpr VectorKey kVectors[] = {
    {"lift", [](Look& look) noexcept { return &look.settings.lift; }},
    {"gamma", [](Look& look) noexcept { return &look.settings.gamma; }},
    {"gain", [](Look& look) noexcept { return &look.settings.gain; }},
    {"shadows-gain", [](Look& look) noexcept { return &look.settings.shadows.gain; }},
    {"shadows-offset", [](Look& look) noexcept { return &look.settings.shadows.offset; }},
    {"midtones-gain", [](Look& look) noexcept { return &look.settings.midtones.gain; }},
    {"midtones-offset", [](Look& look) noexcept { return &look.settings.midtones.offset; }},
    {"highlights-gain", [](Look& look) noexcept { return &look.settings.highlights.gain; }},
    {"highlights-offset", [](Look& look) noexcept { return &look.settings.highlights.offset; }},
    {"mixer-red", [](Look& look) noexcept { return &look.settings.channel_mixer[0]; }},
    {"mixer-green", [](Look& look) noexcept { return &look.settings.channel_mixer[1]; }},
    {"mixer-blue", [](Look& look) noexcept { return &look.settings.channel_mixer[2]; }},
};

[[nodiscard]] std::string_view take(std::string_view& rest) noexcept {
    usize start = 0;
    while (start < rest.size() && (rest[start] == ' ' || rest[start] == '\t')) {
        ++start;
    }
    usize end = start;
    while (end < rest.size() && rest[end] != ' ' && rest[end] != '\t') {
        ++end;
    }
    const std::string_view token = rest.substr(start, end - start);
    rest.remove_prefix(end);
    return token;
}

[[nodiscard]] std::string_view trimmed(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] bool to_number(std::string_view token, f32& out) noexcept {
    char held[64];
    if (token.empty() || token.size() >= sizeof(held)) {
        return false;
    }
    std::memcpy(held, token.data(), token.size());
    held[token.size()] = '\0';
    char* end = nullptr;
    out = std::strtof(held, &end);
    return end == held + token.size() && std::isfinite(out);
}

/// `value` copied into a fixed field, refusing one that does not fit rather than truncating a path.
template <usize N>
[[nodiscard]] bool copy_into(char (&field)[N], std::string_view value) noexcept {
    if (value.size() >= N) {
        return false;
    }
    std::memcpy(field, value.data(), value.size());
    field[value.size()] = '\0';
    return true;
}

[[nodiscard]] Status read_scalar(const ScalarKey& key, std::string_view rest, Look& out) noexcept {
    f32 value = 0.0F;
    if (!to_number(take(rest), value) || !take(rest).empty()) {
        return fail(ErrorCode::InvalidArgument, ".cygrade: a scalar control takes one number");
    }
    // Degrees in the file, radians in the settings: a person types degrees.
    if (key.key == "hue-shift") {
        value *= std::numbers::pi_v<f32> / 180.0F;
    }
    *key.field(out) = value;
    return ok();
}

[[nodiscard]] Status read_vector(const VectorKey& key, std::string_view rest, Look& out) noexcept {
    Vec3 value;
    const bool read = to_number(take(rest), value.x) && to_number(take(rest), value.y) &&
                      to_number(take(rest), value.z);
    if (!read || !take(rest).empty()) {
        return fail(ErrorCode::InvalidArgument,
                    ".cygrade: a per-channel control takes three numbers");
    }
    *key.field(out) = value;
    return ok();
}

[[nodiscard]] Status read_cube_encoding(std::string_view rest, Look& out) noexcept {
    const std::string_view value = trimmed(rest);
    if (value == "srgb") {
        out.cube_encoding = CubeEncoding::Srgb;
    } else if (value == "linear") {
        out.cube_encoding = CubeEncoding::Linear;
    } else {
        return fail(ErrorCode::InvalidArgument, ".cygrade: cube-encoding is `srgb` or `linear`");
    }
    return ok();
}

[[nodiscard]] Status read_lut_size(std::string_view rest, Look& out) noexcept {
    f32 size = 0.0F;
    if (!to_number(take(rest), size) || size < 2.0F || size > static_cast<f32>(kMaxCubeLutSize) ||
        size != std::floor(size)) {
        return fail(ErrorCode::InvalidArgument, ".cygrade: lut-size is a whole number, 2 to 129");
    }
    out.lut_size = static_cast<u32>(size);
    return ok();
}

/// The keys that are not a number or three: text fields and the table's size.
[[nodiscard]] bool read_special(std::string_view keyword, std::string_view rest, Look& out,
                                Status& result) noexcept {
    if (keyword == "name") {
        result = copy_into(out.name, trimmed(rest))
                     ? ok()
                     : fail(ErrorCode::InvalidArgument, ".cygrade: the name is too long");
        return true;
    }
    if (keyword == "cube") {
        const std::string_view path = trimmed(rest);
        result = !path.empty() && copy_into(out.cube, path)
                     ? ok()
                     : fail(ErrorCode::InvalidArgument, ".cygrade: the cube path is empty or long");
        return true;
    }
    if (keyword == "cube-encoding") {
        result = read_cube_encoding(rest, out);
        return true;
    }
    if (keyword == "lut-size") {
        result = read_lut_size(rest, out);
        return true;
    }
    return false;
}

[[nodiscard]] Status read_control(std::string_view keyword, std::string_view rest,
                                  Look& out) noexcept {
    Status special = ok();
    if (read_special(keyword, rest, out, special)) {
        return special;
    }
    for (const ScalarKey& key : kScalars) {
        if (key.key == keyword) {
            return read_scalar(key, rest, out);
        }
    }
    for (const VectorKey& key : kVectors) {
        if (key.key == keyword) {
            return read_vector(key, rest, out);
        }
    }
    return fail(ErrorCode::InvalidArgument, ".cygrade: unknown control");
}

}  // namespace

Status parse_look(std::string_view text, Look& out) noexcept {
    out = Look{};
    bool header = false;
    while (!text.empty()) {
        const usize newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
        if (const usize hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        line = trimmed(line);
        if (line.empty()) {
            continue;
        }
        const std::string_view keyword = take(line);
        if (!header) {
            if (keyword != "cygrade" || trimmed(line) != "1") {
                return fail(ErrorCode::InvalidArgument,
                            ".cygrade: the first line must be `cygrade 1`");
            }
            header = true;
            continue;
        }
        if (Status read = read_control(keyword, line, out); !read) {
            return read;
        }
    }
    if (!header) {
        return fail(ErrorCode::InvalidArgument, ".cygrade: empty — no `cygrade 1` header");
    }
    return ok();
}

}  // namespace cy::rendering
