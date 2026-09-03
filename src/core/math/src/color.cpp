// Color's sRGB boundary. Task 3.1.1. See include/cy/core/math/color.h.

#include <cy/core/math/color.h>

#include <cmath>

namespace cy {
namespace {

/// The sRGB transfer function's break point and its two segments, from IEC 61966-2-1. Written as
/// named constants rather than inline literals because 0.04045 and 0.0031308 are two different
/// numbers for the same break point — one in encoded space and one in linear space — and seeing
/// them next to each other is the only way that reads correctly.
constexpr f32 kEncodedBreak = 0.04045f;
constexpr f32 kLinearBreak = 0.0031308f;
constexpr f32 kLinearSlope = 12.92f;
constexpr f32 kGamma = 2.4f;
constexpr f32 kOffset = 0.055f;

[[nodiscard]] f32 to_unit(u8 value) noexcept {
    return static_cast<f32>(value) * (1.0f / 255.0f);
}

[[nodiscard]] u8 to_byte(f32 value) noexcept {
    // Round to nearest rather than truncate: truncation loses a full level on every channel and
    // makes a round trip through 8 bits drift downward.
    const f32 scaled = math::saturate(value) * 255.0f + 0.5f;
    return static_cast<u8>(scaled);
}

}  // namespace

f32 srgb_to_linear(f32 encoded) noexcept {
    if (encoded <= kEncodedBreak) {
        return encoded / kLinearSlope;
    }
    return std::pow((encoded + kOffset) / (1.0f + kOffset), kGamma);
}

f32 linear_to_srgb(f32 linear) noexcept {
    if (linear <= kLinearBreak) {
        return linear * kLinearSlope;
    }
    return (1.0f + kOffset) * std::pow(linear, 1.0f / kGamma) - kOffset;
}

Color Color::from_srgb8(u8 red, u8 green, u8 blue, u8 alpha) noexcept {
    // Alpha is linear coverage and is *not* gamma-encoded. Running it through the transfer function
    // is the classic mistake and makes every fade non-linear.
    return Color{srgb_to_linear(to_unit(red)), srgb_to_linear(to_unit(green)),
                 srgb_to_linear(to_unit(blue)), to_unit(alpha)};
}

Color Color::from_srgb_hex(u32 rgba) noexcept {
    return from_srgb8(static_cast<u8>((rgba >> 24) & 0xFFu), static_cast<u8>((rgba >> 16) & 0xFFu),
                      static_cast<u8>((rgba >> 8) & 0xFFu), static_cast<u8>(rgba & 0xFFu));
}

void Color::to_srgb8(u8& red, u8& green, u8& blue, u8& alpha) const noexcept {
    red = to_byte(linear_to_srgb(r));
    green = to_byte(linear_to_srgb(g));
    blue = to_byte(linear_to_srgb(b));
    alpha = to_byte(a);
}

}  // namespace cy
