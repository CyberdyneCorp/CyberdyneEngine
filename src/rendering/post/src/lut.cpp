// SPDX-License-Identifier: MIT
#include <cy/rendering/post/lut.h>

#include <cy/core/math/scalar.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace cy::rendering {
namespace {

/// The display encoding's toe, 2⁻¹²: below it the encoding is linear, above it logarithmic.
constexpr f32 kToe = 1.0F / 4096.0F;
/// `log2(1 + 1 / kToe)` = log2(4097), so that display-linear 1 encodes to exactly 1.
constexpr f32 kDisplaySpan = 12.000352177480302F;

// --- `.cube` text ----------------------------------------------------------------------------

/// One whitespace-separated token, consumed from the front of `rest`.
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

/// A whole token as a number, or false. `strtof` wants a terminated string and a token is not one,
/// so it is copied into a buffer first; a token too long for any number is not one.
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

[[nodiscard]] bool to_triple(std::string_view& rest, Vec3& out) noexcept {
    const bool read = to_number(take(rest), out.x) && to_number(take(rest), out.y) &&
                      to_number(take(rest), out.z);
    return read && take(rest).empty();
}

/// A line without its comment, its carriage return and its surrounding blanks.
[[nodiscard]] std::string_view content_of(std::string_view line) noexcept {
    const usize hash = line.find('#');
    if (hash != std::string_view::npos) {
        line = line.substr(0, hash);
    }
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.remove_suffix(1);
    }
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
    }
    return line;
}

/// A keyword line. False with `refusal` set when the line is a keyword the parser refuses.
[[nodiscard]] bool read_keyword(std::string_view keyword, std::string_view rest, CubeLut& out,
                                const char*& refusal) noexcept {
    if (keyword == "TITLE") {
        return true;
    }
    if (keyword == "LUT_1D_SIZE") {
        refusal = ".cube: a 1D LUT is not a colour grade the runtime can apply; export a 3D LUT";
        return false;
    }
    if (keyword == "LUT_3D_SIZE") {
        f32 size = 0.0F;
        if (!to_number(take(rest), size) || size < 2.0F ||
            size > static_cast<f32>(kMaxCubeLutSize) || size != std::floor(size)) {
            refusal = ".cube: LUT_3D_SIZE must be a whole number from 2 to 129";
            return false;
        }
        out.size = static_cast<u32>(size);
        return true;
    }
    if (keyword == "DOMAIN_MIN" || keyword == "DOMAIN_MAX") {
        Vec3& domain = keyword == "DOMAIN_MIN" ? out.domain_min : out.domain_max;
        if (!to_triple(rest, domain)) {
            refusal = ".cube: DOMAIN_MIN and DOMAIN_MAX take three numbers";
            return false;
        }
        return true;
    }
    if (keyword == "LUT_3D_INPUT_RANGE") {
        f32 low = 0.0F;
        f32 high = 1.0F;
        if (!to_number(take(rest), low) || !to_number(take(rest), high)) {
            refusal = ".cube: LUT_3D_INPUT_RANGE takes two numbers";
            return false;
        }
        out.domain_min = Vec3{low, low, low};
        out.domain_max = Vec3{high, high, high};
        return true;
    }
    refusal = ".cube: a line is neither a known keyword nor three numbers";
    return false;
}

[[nodiscard]] bool starts_like_a_number(std::string_view line) noexcept {
    const char first = line.front();
    return (first >= '0' && first <= '9') || first == '-' || first == '+' || first == '.';
}

[[nodiscard]] Status read_line(std::string_view line, CubeLut& out) noexcept {
    if (starts_like_a_number(line)) {
        if (out.size == 0) {
            return fail(ErrorCode::InvalidArgument, ".cube: an entry comes before LUT_3D_SIZE");
        }
        Vec3 entry;
        if (!to_triple(line, entry)) {
            return fail(ErrorCode::InvalidArgument, ".cube: an entry is not three numbers");
        }
        return out.entries.push_back(entry);
    }
    std::string_view rest = line;
    const std::string_view keyword = take(rest);
    const char* refusal = "";
    if (!read_keyword(keyword, rest, out, refusal)) {
        return fail(ErrorCode::InvalidArgument, refusal);
    }
    return ok();
}

[[nodiscard]] f32 encode_for(CubeEncoding encoding, f32 value) noexcept {
    return encoding == CubeEncoding::Srgb ? srgb_encode(value) : math::saturate(value);
}

[[nodiscard]] f32 decode_for(CubeEncoding encoding, f32 value) noexcept {
    return encoding == CubeEncoding::Srgb ? srgb_decode(value) : math::saturate(value);
}

}  // namespace

const char* cube_encoding_name(CubeEncoding encoding) noexcept {
    switch (encoding) {
        case CubeEncoding::Srgb:
            return "srgb";
        case CubeEncoding::Linear:
            return "linear";
        case CubeEncoding::Count:
            break;
    }
    return "unknown";
}

Vec3 CubeLut::sample(Vec3 input) const noexcept {
    const auto normalise = [](f32 value, f32 low, f32 high) noexcept -> f32 {
        const f32 span = high - low;
        return span > 1e-12F ? (value - low) / span : 0.0F;
    };
    const Vec3 encoded{normalise(input.x, domain_min.x, domain_max.x),
                       normalise(input.y, domain_min.y, domain_max.y),
                       normalise(input.z, domain_min.z, domain_max.z)};
    return sample_lut_encoded(entries.data(), size, encoded);
}

Status parse_cube_lut(std::string_view text, CubeLut& out) noexcept {
    out.size = 0;
    out.domain_min = Vec3{0.0F, 0.0F, 0.0F};
    out.domain_max = Vec3{1.0F, 1.0F, 1.0F};
    out.entries.clear();
    while (!text.empty()) {
        const usize newline = text.find('\n');
        const std::string_view line = content_of(text.substr(0, newline));
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
        if (line.empty()) {
            continue;
        }
        if (Status read = read_line(line, out); !read) {
            return read;
        }
    }
    if (out.size == 0) {
        return fail(ErrorCode::InvalidArgument, ".cube: no LUT_3D_SIZE");
    }
    const usize expected = static_cast<usize>(out.size) * out.size * out.size;
    if (out.entries.size() != expected) {
        return fail(ErrorCode::InvalidArgument, ".cube: the entry count is not LUT_3D_SIZE cubed");
    }
    const bool ordered = out.domain_max.x > out.domain_min.x &&
                         out.domain_max.y > out.domain_min.y && out.domain_max.z > out.domain_min.z;
    if (!ordered) {
        return fail(ErrorCode::InvalidArgument, ".cube: DOMAIN_MAX is not above DOMAIN_MIN");
    }
    return ok();
}

f32 srgb_encode(f32 linear) noexcept {
    const f32 value = math::saturate(linear);
    return value <= 0.0031308F ? value * 12.92F : (1.055F * std::pow(value, 1.0F / 2.4F)) - 0.055F;
}

f32 srgb_decode(f32 encoded) noexcept {
    const f32 value = math::saturate(encoded);
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

f32 display_log_encode(f32 display_linear) noexcept {
    return std::log2(1.0F + (math::saturate(display_linear) / kToe)) / kDisplaySpan;
}

f32 display_log_decode(f32 encoded) noexcept {
    return kToe * (std::exp2(math::saturate(encoded) * kDisplaySpan) - 1.0F);
}

Vec3 display_log_encode(Vec3 display_linear) noexcept {
    return Vec3{display_log_encode(display_linear.x), display_log_encode(display_linear.y),
                display_log_encode(display_linear.z)};
}

Vec3 display_log_decode(Vec3 encoded) noexcept {
    return Vec3{display_log_decode(encoded.x), display_log_decode(encoded.y),
                display_log_decode(encoded.z)};
}

Vec3 apply_display_grade(Vec3 display_linear, const DisplayGrade& grade) noexcept {
    // A neutral parametric grade is skipped rather than evaluated: evaluated, contrast about middle
    // grey and saturation about luminance each round in the last bit, and "neutral" should mean the
    // colour comes back as it went in.
    Vec3 graded =
        grade.settings.neutral() ? display_linear : apply_grading(display_linear, grade.settings);
    graded = Vec3{math::saturate(graded.x), math::saturate(graded.y), math::saturate(graded.z)};
    if (grade.cube == nullptr || grade.cube->size < 2) {
        return graded;
    }
    const CubeEncoding encoding = grade.cube_encoding;
    const Vec3 input{encode_for(encoding, graded.x), encode_for(encoding, graded.y),
                     encode_for(encoding, graded.z)};
    const Vec3 looked = grade.cube->sample(input);
    return Vec3{decode_for(encoding, looked.x), decode_for(encoding, looked.y),
                decode_for(encoding, looked.z)};
}

bool bake_display_lut(const DisplayGrade& grade, u32 size, Vec3* out, usize capacity) noexcept {
    if (out == nullptr || size < 2) {
        return false;
    }
    const usize needed = static_cast<usize>(size) * size * size;
    if (capacity < needed) {
        return false;
    }
    const f32 step = 1.0F / static_cast<f32>(size - 1U);
    usize index = 0;
    for (u32 b = 0; b < size; ++b) {
        for (u32 g = 0; g < size; ++g) {
            for (u32 r = 0; r < size; ++r) {
                const Vec3 lattice{static_cast<f32>(r) * step, static_cast<f32>(g) * step,
                                   static_cast<f32>(b) * step};
                // STORED ENCODED: interpolating the encoded output is exact for any grade that is
                // linear in the encoding — the identity, a channel permutation, a gain in the log
                // region — where interpolating display-linear outputs across a 0.375-stop cell is
                // wrong by up to 0.8 %, two 8-bit steps at white.
                out[index++] =
                    display_log_encode(apply_display_grade(display_log_decode(lattice), grade));
            }
        }
    }
    return true;
}

Vec3 sample_display_lut(const Vec3* lut, u32 size, Vec3 display_linear) noexcept {
    if (lut == nullptr || size < 2) {
        return display_linear;
    }
    return display_log_decode(sample_lut_encoded(lut, size, display_log_encode(display_linear)));
}

bool display_lut_is_identity(const Vec3* lut, u32 size, f32 tolerance) noexcept {
    if (lut == nullptr || size < 2) {
        return false;
    }
    const f32 step = 1.0F / static_cast<f32>(size - 1U);
    usize index = 0;
    for (u32 b = 0; b < size; ++b) {
        for (u32 g = 0; g < size; ++g) {
            for (u32 r = 0; r < size; ++r) {
                const Vec3 lattice{static_cast<f32>(r) * step, static_cast<f32>(g) * step,
                                   static_cast<f32>(b) * step};
                const Vec3 entry = lut[index++];
                const bool same = std::fabs(entry.x - lattice.x) <= tolerance &&
                                  std::fabs(entry.y - lattice.y) <= tolerance &&
                                  std::fabs(entry.z - lattice.z) <= tolerance;
                if (!same) {
                    return false;
                }
            }
        }
    }
    return true;
}

}  // namespace cy::rendering
