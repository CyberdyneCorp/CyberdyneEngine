#include <cy/rendering/post/grading.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering {
namespace {

/// The log encoding's range, in stops around middle grey. Eighteen stops covers everything a scene
/// referred pipeline produces after exposure without wasting resolution on values nothing reaches.
constexpr f32 kLogMin = -10.0F;
constexpr f32 kLogMax = 8.0F;
constexpr f32 kLogSpan = kLogMax - kLogMin;

[[nodiscard]] f32 luminance_of(Vec3 colour) noexcept {
    return (0.2126F * colour.x) + (0.7152F * colour.y) + (0.0722F * colour.z);
}

/// A cheap Kelvin-to-RGB gain. Exact black-body conversion belongs to
/// `rendering-lighting-and-shadows`' `units.h`; what grading needs is a smooth, monotone warm-cool
/// axis through 6500 K, and a second copy of the physical model here would be one to keep in step.
[[nodiscard]] Vec3 white_balance_gain(f32 temperature, f32 tint) noexcept {
    const f32 t = math::clamp((temperature - 6500.0F) / 6500.0F, -1.0F, 1.0F);
    Vec3 gain{1.0F - (t * 0.25F), 1.0F, 1.0F + (t * 0.25F)};
    gain.y += tint * 0.15F;
    gain.x -= tint * 0.075F;
    gain.z -= tint * 0.075F;
    return gain;
}

[[nodiscard]] Vec3 mix_channels(Vec3 colour, const Vec3 (&mixer)[3]) noexcept {
    return Vec3{dot(mixer[0], colour), dot(mixer[1], colour), dot(mixer[2], colour)};
}

[[nodiscard]] f32 lift_gamma_gain(f32 value, f32 lift, f32 gamma, f32 gain) noexcept {
    const f32 lifted = (value * (1.0F - lift)) + lift;
    const f32 exponent = math::max(gamma, 1e-3F);
    return std::pow(math::max(lifted, 0.0F), 1.0F / exponent) * gain;
}

/// The three wheels' weights for a luminance. They sum to one at every luminance, which is what
/// stops the midtone wheel darkening a scene simply by existing.
void wheel_weights(f32 luminance, f32 shadow_boundary, f32 highlight_boundary, f32& shadows,
                   f32& midtones, f32& highlights) noexcept {
    const f32 low = math::max(shadow_boundary, 1e-3F);
    const f32 high = math::max(highlight_boundary, low + 1e-3F);
    shadows = 1.0F - math::saturate(luminance / low);
    highlights = math::saturate((luminance - high) / math::max(1.0F - high, 1e-3F));
    midtones = math::max(1.0F - shadows - highlights, 0.0F);
}

[[nodiscard]] Vec3 apply_wheels(Vec3 colour, const GradingSettings& settings) noexcept {
    f32 shadows = 0.0F;
    f32 midtones = 0.0F;
    f32 highlights = 0.0F;
    wheel_weights(luminance_of(colour), settings.shadow_boundary, settings.highlight_boundary,
                  shadows, midtones, highlights);
    const Vec3 gain = (settings.shadows.gain * shadows) + (settings.midtones.gain * midtones) +
                      (settings.highlights.gain * highlights);
    const Vec3 offset = (settings.shadows.offset * shadows) +
                        (settings.midtones.offset * midtones) +
                        (settings.highlights.offset * highlights);
    return Vec3{(colour.x * gain.x) + offset.x, (colour.y * gain.y) + offset.y,
                (colour.z * gain.z) + offset.z};
}

/// A hue rotation about the neutral axis. The Rodrigues form rather than an RGB-to-HSV round trip:
/// HSV's hue is undefined for a grey and a grade that produced a NaN on a grey wall would be found
/// by an artist rather than by a test.
[[nodiscard]] Vec3 rotate_hue(Vec3 colour, f32 radians) noexcept {
    if (math::nearly_zero(radians)) {
        return colour;
    }
    const f32 cosine = std::cos(radians);
    const f32 sine = std::sin(radians);
    const f32 third = 1.0F / 3.0F;
    const f32 root = std::sqrt(third);
    const Vec3 axis{root, root, root};
    const f32 projection = dot(colour, axis);
    const Vec3 parallel = axis * projection;
    const Vec3 perpendicular = colour - parallel;
    const Vec3 crossed = cross(axis, colour);
    return parallel + (perpendicular * cosine) + (crossed * sine);
}

}  // namespace

bool GradingSettings::neutral() const noexcept {
    const bool balance = math::nearly_equal(temperature, 6500.0F, 1.0F) && math::nearly_zero(tint);
    const bool curves = lift == Vec3{0.0F, 0.0F, 0.0F} && gamma == Vec3{1.0F, 1.0F, 1.0F} &&
                        gain == Vec3{1.0F, 1.0F, 1.0F};
    const bool wheels =
        shadows.gain == Vec3{1.0F, 1.0F, 1.0F} && midtones.gain == Vec3{1.0F, 1.0F, 1.0F} &&
        highlights.gain == Vec3{1.0F, 1.0F, 1.0F} && shadows.offset == Vec3{0.0F, 0.0F, 0.0F} &&
        midtones.offset == Vec3{0.0F, 0.0F, 0.0F} && highlights.offset == Vec3{0.0F, 0.0F, 0.0F};
    const bool tone = math::nearly_equal(saturation, 1.0F) && math::nearly_equal(contrast, 1.0F) &&
                      math::nearly_zero(hue_shift);
    const bool mixer = channel_mixer[0] == Vec3{1.0F, 0.0F, 0.0F} &&
                       channel_mixer[1] == Vec3{0.0F, 1.0F, 0.0F} &&
                       channel_mixer[2] == Vec3{0.0F, 0.0F, 1.0F};
    return balance && curves && wheels && tone && mixer;
}

f32 log_encode(f32 linear) noexcept {
    const f32 stops = std::log2(math::max(linear, 1e-6F));
    return math::saturate((stops - kLogMin) / kLogSpan);
}

f32 log_decode(f32 encoded) noexcept {
    return std::exp2((math::saturate(encoded) * kLogSpan) + kLogMin);
}

Vec3 log_encode(Vec3 linear) noexcept {
    return Vec3{log_encode(linear.x), log_encode(linear.y), log_encode(linear.z)};
}

Vec3 log_decode(Vec3 encoded) noexcept {
    return Vec3{log_decode(encoded.x), log_decode(encoded.y), log_decode(encoded.z)};
}

Vec3 apply_grading(Vec3 colour, const GradingSettings& settings) noexcept {
    const Vec3 balance = white_balance_gain(settings.temperature, settings.tint);
    Vec3 result{colour.x * balance.x, colour.y * balance.y, colour.z * balance.z};
    result = mix_channels(result, settings.channel_mixer);
    result = Vec3{lift_gamma_gain(result.x, settings.lift.x, settings.gamma.x, settings.gain.x),
                  lift_gamma_gain(result.y, settings.lift.y, settings.gamma.y, settings.gain.y),
                  lift_gamma_gain(result.z, settings.lift.z, settings.gamma.z, settings.gain.z)};
    result = apply_wheels(result, settings);

    // Contrast about middle grey, so raising it does not also raise exposure.
    constexpr f32 middle_grey = 0.18F;
    result = Vec3{((result.x - middle_grey) * settings.contrast) + middle_grey,
                  ((result.y - middle_grey) * settings.contrast) + middle_grey,
                  ((result.z - middle_grey) * settings.contrast) + middle_grey};

    const f32 grey = luminance_of(result);
    result = Vec3{math::lerp(grey, result.x, settings.saturation),
                  math::lerp(grey, result.y, settings.saturation),
                  math::lerp(grey, result.z, settings.saturation)};
    result = rotate_hue(result, settings.hue_shift);
    return Vec3{math::max(result.x, 0.0F), math::max(result.y, 0.0F), math::max(result.z, 0.0F)};
}

bool bake_grading_lut(const GradingSettings& settings, u32 size, Vec3* out,
                      usize capacity) noexcept {
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
                // Sampled in LOG space: the table's axes are log-encoded values, which is where the
                // shadows have resolution to spend.
                const Vec3 encoded{static_cast<f32>(r) * step, static_cast<f32>(g) * step,
                                   static_cast<f32>(b) * step};
                out[index] = apply_grading(log_decode(encoded), settings);
                ++index;
            }
        }
    }
    return true;
}

Vec3 sample_grading_lut(const Vec3* lut, u32 size, Vec3 colour) noexcept {
    if (lut == nullptr || size < 2) {
        return colour;
    }
    const Vec3 encoded = log_encode(colour);
    const f32 last = static_cast<f32>(size - 1U);
    const f32 fx = math::saturate(encoded.x) * last;
    const f32 fy = math::saturate(encoded.y) * last;
    const f32 fz = math::saturate(encoded.z) * last;
    const u32 x0 = static_cast<u32>(fx);
    const u32 y0 = static_cast<u32>(fy);
    const u32 z0 = static_cast<u32>(fz);
    const u32 x1 = math::min(x0 + 1U, size - 1U);
    const u32 y1 = math::min(y0 + 1U, size - 1U);
    const u32 z1 = math::min(z0 + 1U, size - 1U);
    const f32 tx = fx - static_cast<f32>(x0);
    const f32 ty = fy - static_cast<f32>(y0);
    const f32 tz = fz - static_cast<f32>(z0);

    const auto fetch = [lut, size](u32 x, u32 y, u32 z) noexcept -> Vec3 {
        return lut[(static_cast<usize>(z) * size * size) + (static_cast<usize>(y) * size) + x];
    };
    const auto blend = [](Vec3 a, Vec3 b, f32 t) noexcept -> Vec3 {
        return Vec3{math::lerp(a.x, b.x, t), math::lerp(a.y, b.y, t), math::lerp(a.z, b.z, t)};
    };

    const Vec3 c00 = blend(fetch(x0, y0, z0), fetch(x1, y0, z0), tx);
    const Vec3 c10 = blend(fetch(x0, y1, z0), fetch(x1, y1, z0), tx);
    const Vec3 c01 = blend(fetch(x0, y0, z1), fetch(x1, y0, z1), tx);
    const Vec3 c11 = blend(fetch(x0, y1, z1), fetch(x1, y1, z1), tx);
    return blend(blend(c00, c10, ty), blend(c01, c11, ty), tz);
}

}  // namespace cy::rendering
