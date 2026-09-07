#include <cy/rendering/post/tonemap.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering {
namespace {

[[nodiscard]] f32 luminance_of(Vec3 colour) noexcept {
    return (0.2126F * colour.x) + (0.7152F * colour.y) + (0.0722F * colour.z);
}

[[nodiscard]] Vec3 saturate3(Vec3 colour) noexcept {
    return Vec3{math::saturate(colour.x), math::saturate(colour.y), math::saturate(colour.z)};
}

/// The ACES RRT+ODT fit. Applied per channel, which is what the fit is, and which is also why it
/// shifts hue in the highlights — the thing AgX is the default to avoid.
[[nodiscard]] f32 aces_channel(f32 x) noexcept {
    constexpr f32 a = 2.51F;
    constexpr f32 b = 0.03F;
    constexpr f32 c = 2.43F;
    constexpr f32 d = 0.59F;
    constexpr f32 e = 0.14F;
    return math::saturate((x * ((a * x) + b)) / ((x * ((c * x) + d)) + e));
}

/// AgX's sigmoid on the log-encoded value. The log encoding is what makes the curve act on stops
/// rather than on linear values, and it is the reason the operator's highlight behaviour is the
/// same at every exposure.
[[nodiscard]] f32 agx_curve(f32 log_value) noexcept {
    const f32 x = math::saturate((log_value + 12.47393F) / (4.026069F + 12.47393F));
    // A smooth polynomial through the AgX contrast points. Monotone on [0,1], which matters: a
    // non-monotone tone curve inverts a gradient somewhere and the artefact looks like banding.
    const f32 x2 = x * x;
    const f32 x3 = x2 * x;
    return math::saturate((3.0F * x2) - (2.0F * x3));
}

[[nodiscard]] Vec3 apply_agx(Vec3 colour) noexcept {
    // AgX's hue stability comes from desaturating TOWARD the tonemapped luminance rather than
    // tonemapping each channel independently. The amount rises with how far into the highlights the
    // colour is, which is the "gentle highlight desaturation" the specification asks for by name.
    const Vec3 clamped{math::max(colour.x, 0.0F), math::max(colour.y, 0.0F),
                       math::max(colour.z, 0.0F)};
    const f32 luminance = math::max(luminance_of(clamped), 1e-6F);
    const f32 mapped = agx_curve(std::log2(luminance));

    const f32 ratio = mapped / luminance;
    const Vec3 scaled{clamped.x * ratio, clamped.y * ratio, clamped.z * ratio};
    const f32 highlight = math::saturate((mapped - 0.5F) * 2.0F);
    const f32 pull = highlight * highlight * 0.85F;
    return saturate3(Vec3{math::lerp(scaled.x, mapped, pull), math::lerp(scaled.y, mapped, pull),
                          math::lerp(scaled.z, mapped, pull)});
}

[[nodiscard]] f32 pq_encode(f32 nits) noexcept {
    constexpr f32 m1 = 0.1593017578125F;
    constexpr f32 m2 = 78.84375F;
    constexpr f32 c1 = 0.8359375F;
    constexpr f32 c2 = 18.8515625F;
    constexpr f32 c3 = 18.6875F;
    const f32 y = math::saturate(nits / 10000.0F);
    const f32 ym = std::pow(y, m1);
    return std::pow((c1 + (c2 * ym)) / (1.0F + (c3 * ym)), m2);
}

[[nodiscard]] f32 srgb_encode(f32 value) noexcept {
    const f32 x = math::saturate(value);
    return x <= 0.0031308F ? x * 12.92F : (1.055F * std::pow(x, 1.0F / 2.4F)) - 0.055F;
}

}  // namespace

const char* tonemap_operator_name(TonemapOperator op) noexcept {
    switch (op) {
        case TonemapOperator::None:
            return "None";
        case TonemapOperator::Reinhard:
            return "Reinhard";
        case TonemapOperator::ReinhardExtended:
            return "ReinhardExtended";
        case TonemapOperator::Aces:
            return "ACES";
        case TonemapOperator::AgX:
            return "AgX";
        case TonemapOperator::Custom:
            return "Custom";
        case TonemapOperator::Count:
            break;
    }
    return "Unknown";
}

const char* transfer_function_name(TransferFunction function) noexcept {
    switch (function) {
        case TransferFunction::Srgb:
            return "sRGB";
        case TransferFunction::Pq:
            return "PQ";
        case TransferFunction::ScRgb:
            return "scRGB";
        case TransferFunction::Count:
            break;
    }
    return "Unknown";
}

Vec3 tonemap(Vec3 colour, const TonemapSettings& settings) noexcept {
    // An HDR display is mapped to ITS range rather than to SDR white, which is the requirement's
    // second scenario. The headroom is expressed as a scale on the input so that the operator's
    // shoulder lands where the display's peak is.
    const f32 headroom = settings.output.transfer == TransferFunction::Srgb
                             ? 1.0F
                             : math::max(settings.output.peak_nits, 100.0F) / 100.0F;
    const Vec3 input{colour.x / headroom, colour.y / headroom, colour.z / headroom};

    Vec3 mapped;
    switch (settings.op) {
        case TonemapOperator::None:
            mapped = saturate3(input);
            break;
        case TonemapOperator::Reinhard:
            mapped = saturate3(Vec3{input.x / (1.0F + input.x), input.y / (1.0F + input.y),
                                    input.z / (1.0F + input.z)});
            break;
        case TonemapOperator::ReinhardExtended: {
            const f32 white = math::max(settings.white_point, 1e-3F);
            const f32 inverse = 1.0F / (white * white);
            // Clamped at the white point: extended Reinhard maps `white_point` to one and takes
            // everything above it past one, which is a value no display can show and which the
            // stages after this one would carry into the encode.
            mapped = saturate3(Vec3{input.x * (1.0F + (input.x * inverse)) / (1.0F + input.x),
                                    input.y * (1.0F + (input.y * inverse)) / (1.0F + input.y),
                                    input.z * (1.0F + (input.z * inverse)) / (1.0F + input.z)});
            break;
        }
        case TonemapOperator::Aces:
            mapped = Vec3{aces_channel(input.x), aces_channel(input.y), aces_channel(input.z)};
            break;
        case TonemapOperator::AgX:
            mapped = apply_agx(input);
            break;
        case TonemapOperator::Custom:
        case TonemapOperator::Count:
            // The caller supplies the curve or LUT. Passing the input through unchanged is the
            // honest behaviour: silently substituting AgX would hide a project's missing asset.
            mapped = input;
            break;
    }
    return Vec3{mapped.x * headroom, mapped.y * headroom, mapped.z * headroom};
}

Vec3 encode_output(Vec3 display_referred, const OutputTarget& target) noexcept {
    switch (target.transfer) {
        case TransferFunction::Srgb:
            return Vec3{srgb_encode(display_referred.x), srgb_encode(display_referred.y),
                        srgb_encode(display_referred.z)};
        case TransferFunction::Pq: {
            const f32 scale = math::max(target.peak_nits, 1.0F);
            return Vec3{pq_encode(display_referred.x * scale),
                        pq_encode(display_referred.y * scale),
                        pq_encode(display_referred.z * scale)};
        }
        case TransferFunction::ScRgb:
        case TransferFunction::Count:
            // Linear, extended range: the display takes the value as it is.
            return display_referred;
    }
    return display_referred;
}

f32 colour_saturation(Vec3 colour) noexcept {
    const f32 high = math::max(colour.x, math::max(colour.y, colour.z));
    const f32 low = math::min(colour.x, math::min(colour.y, colour.z));
    return high <= 1e-6F ? 0.0F : (high - low) / high;
}

f32 colour_hue(Vec3 colour) noexcept {
    const f32 high = math::max(colour.x, math::max(colour.y, colour.z));
    const f32 low = math::min(colour.x, math::min(colour.y, colour.z));
    const f32 span = high - low;
    if (span <= 1e-6F) {
        return 0.0F;
    }
    f32 hue = 0.0F;
    if (high == colour.x) {
        hue = (colour.y - colour.z) / span;
    } else if (high == colour.y) {
        hue = 2.0F + ((colour.z - colour.x) / span);
    } else {
        hue = 4.0F + ((colour.x - colour.y) / span);
    }
    hue *= math::kPi / 3.0F;
    return hue < 0.0F ? hue + math::kTwoPi : hue;
}

}  // namespace cy::rendering
