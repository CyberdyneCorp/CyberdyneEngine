// SPDX-License-Identifier: MIT
// Easing curves and colour interpolation. See animation.h.

#include <cy/ui/toolkit/animation.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cy::ui {
namespace {

/// A unit cubic Bezier from (0, 0) to (1, 1) with control points (x1, y1) and (x2, y2), evaluated
/// at x by Newton's method on the x polynomial — exactly how a browser evaluates CSS's easings.
[[nodiscard]] f32 cubic_bezier(f32 x1, f32 y1, f32 x2, f32 y2, f32 x) noexcept {
    const auto sample = [](f32 a1, f32 a2, f32 t) noexcept {
        const f32 u = 1.0F - t;
        return (3.0F * u * u * t * a1) + (3.0F * u * t * t * a2) + (t * t * t);
    };
    const auto slope = [](f32 a1, f32 a2, f32 t) noexcept {
        const f32 u = 1.0F - t;
        return (3.0F * u * u * a1) + (6.0F * u * t * (a2 - a1)) + (3.0F * t * t * (1.0F - a2));
    };
    f32 t = x;
    for (u32 iteration = 0; iteration < 8U; ++iteration) {
        const f32 error = sample(x1, x2, t) - x;
        const f32 derivative = slope(x1, x2, t);
        if (std::fabs(error) < 1e-5F || std::fabs(derivative) < 1e-6F) {
            break;
        }
        t = std::clamp(t - (error / derivative), 0.0F, 1.0F);
    }
    return sample(y1, y2, t);
}

}  // namespace

const char* easing_name(Easing easing) noexcept {
    switch (easing) {
        case Easing::Linear:
            return "linear";
        case Easing::EaseIn:
            return "ease-in";
        case Easing::EaseOut:
            return "ease-out";
        case Easing::EaseInOut:
            return "ease-in-out";
        case Easing::Ease:
            return "ease";
        case Easing::Spring:
            return "spring";
        case Easing::Back:
            return "back";
        case Easing::Count:
            break;
    }
    return "unknown";
}

Easing easing_from_name(std::string_view name) noexcept {
    for (u32 index = 0; index < static_cast<u32>(Easing::Count); ++index) {
        const auto easing = static_cast<Easing>(index);
        if (name == easing_name(easing)) {
            return easing;
        }
    }
    return Easing::Ease;
}

f32 ease(Easing easing, f32 t) noexcept {
    const f32 x = std::clamp(t, 0.0F, 1.0F);
    if (x <= 0.0F) {
        return 0.0F;
    }
    if (x >= 1.0F) {
        return 1.0F;
    }
    switch (easing) {
        case Easing::Linear:
            return x;
        case Easing::EaseIn:
            return cubic_bezier(0.42F, 0.0F, 1.0F, 1.0F, x);
        case Easing::EaseOut:
            return cubic_bezier(0.0F, 0.0F, 0.58F, 1.0F, x);
        case Easing::EaseInOut:
            return cubic_bezier(0.42F, 0.0F, 0.58F, 1.0F, x);
        case Easing::Ease:
            return cubic_bezier(0.25F, 0.1F, 0.25F, 1.0F, x);
        case Easing::Spring: {
            // An under-damped oscillator released from rest at zero towards one: about one and a
            // half visible swings, its envelope scaled so the curve is within 0.1 % of one at the
            // end, and the clamp above makes the end exactly one.
            constexpr f32 kDamping = 6.9F;
            constexpr f32 kFrequency = 4.5F * std::numbers::pi_v<f32>;
            return 1.0F - (std::exp(-kDamping * x) * std::cos(kFrequency * x));
        }
        case Easing::Back: {
            constexpr f32 kOvershoot = 1.70158F;
            const f32 u = x - 1.0F;
            return 1.0F + (u * u * (((kOvershoot + 1.0F) * u) + kOvershoot));
        }
        case Easing::Count:
            break;
    }
    return x;
}

const char* animated_property_name(AnimatedProperty property) noexcept {
    switch (property) {
        case AnimatedProperty::Opacity:
            return "opacity";
        case AnimatedProperty::TranslateX:
            return "translate-x";
        case AnimatedProperty::TranslateY:
            return "translate-y";
        case AnimatedProperty::Scale:
            return "scale";
        case AnimatedProperty::Background:
            return "background-color";
        case AnimatedProperty::Count:
            break;
    }
    return "unknown";
}

u32 mix_colour(u32 from, u32 to, f32 t) noexcept {
    const f32 x = std::clamp(t, 0.0F, 1.0F);
    u32 out = 0;
    for (u32 shift = 0; shift < 32U; shift += 8U) {
        const auto a = static_cast<f32>((from >> shift) & 0xFFU);
        const auto b = static_cast<f32>((to >> shift) & 0xFFU);
        const auto value = static_cast<u32>(std::lround(a + ((b - a) * x)));
        out |= std::min(value, 255U) << shift;
    }
    return out;
}

}  // namespace cy::ui
