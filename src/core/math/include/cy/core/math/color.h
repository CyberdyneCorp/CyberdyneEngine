#pragma once
// Color — linear RGBA, and the sRGB boundary. Task 3.1.1.
//
// `core-math` — "Math types" lists `Color`. What the list does not say, and what matters more than
// the type, is which space the four floats are in: **linear**. Lighting, blending and filtering are
// only correct in a linear space, and an engine that stores sRGB values in a float and lights with
// them produces the dark, over-saturated look that is the signature of getting this wrong.
//
// sRGB appears at two boundaries and nowhere else: an 8-bit texture or colour picked by an artist
// comes *in* through `from_srgb8`, and a final image goes *out* through the swapchain's own sRGB
// encoding. Both conversions are named for the space they leave, so a call site that mentions sRGB
// is a boundary and one that does not is interior.

#include <cy/core/base/types.h>
#include <cy/core/math/scalar.h>
#include <cy/core/math/vec.h>

namespace cy {

/// Linear, un-premultiplied RGBA. Alpha is linear coverage and is never gamma-encoded, which is why
/// `from_srgb8` converts three channels and copies the fourth.
struct Color {
    f32 r = 0.0f;
    f32 g = 0.0f;
    f32 b = 0.0f;
    f32 a = 1.0f;

    [[nodiscard]] static constexpr Color from_linear(f32 red, f32 green, f32 blue,
                                                     f32 alpha = 1.0f) noexcept {
        return Color{red, green, blue, alpha};
    }

    /// An 8-bit sRGB colour — a texel, a hex literal from a designer, a colour picker's output —
    /// converted to the linear space the engine works in.
    [[nodiscard]] static Color from_srgb8(u8 red, u8 green, u8 blue, u8 alpha = 255) noexcept;

    /// 0xRRGGBBAA, the order a CSS-style hex literal is written in.
    [[nodiscard]] static Color from_srgb_hex(u32 rgba) noexcept;

    /// Back to 8-bit sRGB, for a colour swatch in the editor or a screenshot's pixel.
    void to_srgb8(u8& red, u8& green, u8& blue, u8& alpha) const noexcept;

    [[nodiscard]] constexpr Vec4 to_vec4() const noexcept { return Vec4{r, g, b, a}; }
    [[nodiscard]] constexpr Vec3 rgb() const noexcept { return Vec3{r, g, b}; }

    /// Rec. 709 luminance of the linear colour. The coefficients are the ones the engine's colour
    /// primaries imply; they are not a stylistic choice.
    [[nodiscard]] constexpr f32 luminance() const noexcept {
        return 0.2126f * r + 0.7152f * g + 0.0722f * b;
    }

    [[nodiscard]] constexpr Color with_alpha(f32 alpha) const noexcept {
        return Color{r, g, b, alpha};
    }
};

static_assert(sizeof(Color) == 16);

[[nodiscard]] constexpr bool operator==(const Color& x, const Color& y) noexcept {
    return x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a;
}
[[nodiscard]] constexpr bool operator!=(const Color& x, const Color& y) noexcept {
    return !(x == y);
}

[[nodiscard]] constexpr Color operator*(const Color& c, f32 s) noexcept {
    return Color{c.r * s, c.g * s, c.b * s, c.a * s};
}
[[nodiscard]] constexpr Color operator+(const Color& x, const Color& y) noexcept {
    return Color{x.r + y.r, x.g + y.g, x.b + y.b, x.a + y.a};
}
[[nodiscard]] constexpr Color cwise_mul(const Color& x, const Color& y) noexcept {
    return Color{x.r * y.r, x.g * y.g, x.b * y.b, x.a * y.a};
}

/// Interpolation in linear space, which is the only place it is physically meaningful. A gradient
/// authored in sRGB and interpolated there passes through different intermediate colours; the
/// `Gradient` in curve.h converts on ingest so that its keys are linear before anything blends
/// them.
[[nodiscard]] constexpr Color lerp(const Color& x, const Color& y, f32 t) noexcept {
    return Color{math::lerp(x.r, y.r, t), math::lerp(x.g, y.g, t), math::lerp(x.b, y.b, t),
                 math::lerp(x.a, y.a, t)};
}

/// One channel, sRGB-encoded [0,1] to linear [0,1]. The piecewise curve, not the 2.2 power
/// approximation: the linear segment near black is what keeps dark values from quantising badly,
/// and the approximation is visibly wrong exactly there.
[[nodiscard]] f32 srgb_to_linear(f32 encoded) noexcept;

/// The inverse of `srgb_to_linear`.
[[nodiscard]] f32 linear_to_srgb(f32 linear) noexcept;

namespace colors {

inline constexpr Color kBlack{0.0f, 0.0f, 0.0f, 1.0f};
inline constexpr Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};
inline constexpr Color kRed{1.0f, 0.0f, 0.0f, 1.0f};
inline constexpr Color kGreen{0.0f, 1.0f, 0.0f, 1.0f};
inline constexpr Color kBlue{0.0f, 0.0f, 1.0f, 1.0f};
inline constexpr Color kTransparent{0.0f, 0.0f, 0.0f, 0.0f};

}  // namespace colors

}  // namespace cy
