// The shared easing table. Task 3.1.5. See include/cy/core/math/easing.h.
//
// Only the `In` form of each family is written out. `Out`, `InOut` and `OutIn` are derived from it
// by the three standard reflections, as function templates parameterised on the `In` function, so a
// family cannot disagree with its own variants — the classic bug where `easeOutBounce` and
// `easeInBounce` were transcribed from different sources and are not each other's mirror.

#include <cy/core/math/easing.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::ease {
namespace {

// --- The In forms -----------------------------------------------------------------------------

f32 linear_in(f32 t) noexcept {
    return t;
}

f32 sine_in(f32 t) noexcept {
    return 1.0f - std::cos(t * math::kHalfPi);
}

f32 quad_in(f32 t) noexcept {
    return t * t;
}
f32 cubic_in(f32 t) noexcept {
    return t * t * t;
}
f32 quart_in(f32 t) noexcept {
    return t * t * t * t;
}
f32 quint_in(f32 t) noexcept {
    return t * t * t * t * t;
}

f32 expo_in(f32 t) noexcept {
    // Anchored at exactly 0, because 2^-10 is 0.00098 and an animation that starts one thousandth
    // of the way along is visibly not starting from rest.
    return t <= 0.0f ? 0.0f : std::pow(2.0f, (10.0f * t) - 10.0f);
}

f32 circ_in(f32 t) noexcept {
    return 1.0f - std::sqrt(math::max(0.0f, 1.0f - (t * t)));
}

f32 back_in(f32 t) noexcept {
    // 1.70158 is the overshoot that makes the curve pull back by about 10% before advancing. It is
    // the value Robert Penner's original table used and every implementation since has kept, so
    // changing it would make the engine's "back" ease subtly unlike every tool a designer knows.
    constexpr f32 kOvershoot = 1.70158f;
    return t * t * (((kOvershoot + 1.0f) * t) - kOvershoot);
}

f32 elastic_in(f32 t) noexcept {
    constexpr f32 kPeriod = 2.0f * math::kPi / 3.0f;
    if (t <= 0.0f) {
        return 0.0f;
    }
    if (t >= 1.0f) {
        return 1.0f;
    }
    return -std::pow(2.0f, (10.0f * t) - 10.0f) * std::sin(((t * 10.0f) - 10.75f) * kPeriod);
}

/// Bounce is conventionally *defined* as its Out form — a ball dropping and rebounding — so the In
/// form is the reflection rather than the other way round. Deriving Out from this In therefore
/// gives back the canonical curve exactly.
f32 bounce_out_raw(f32 t) noexcept {
    constexpr f32 n1 = 7.5625f;
    constexpr f32 d1 = 2.75f;
    if (t < 1.0f / d1) {
        return n1 * t * t;
    }
    if (t < 2.0f / d1) {
        const f32 shifted = t - (1.5f / d1);
        return (n1 * shifted * shifted) + 0.75f;
    }
    if (t < 2.5f / d1) {
        const f32 shifted = t - (2.25f / d1);
        return (n1 * shifted * shifted) + 0.9375f;
    }
    const f32 shifted = t - (2.625f / d1);
    return (n1 * shifted * shifted) + 0.984375f;
}

f32 bounce_in(f32 t) noexcept {
    return 1.0f - bounce_out_raw(1.0f - t);
}

// --- The three reflections ----------------------------------------------------------------------

template <f32 (*In)(f32)>
f32 out_of(f32 t) noexcept {
    return 1.0f - In(1.0f - t);
}

template <f32 (*In)(f32)>
f32 in_out_of(f32 t) noexcept {
    return t < 0.5f ? In(t * 2.0f) * 0.5f : 1.0f - (In(2.0f - (t * 2.0f)) * 0.5f);
}

template <f32 (*In)(f32)>
f32 out_in_of(f32 t) noexcept {
    return t < 0.5f ? out_of<In>(t * 2.0f) * 0.5f : 0.5f + (In((t * 2.0f) - 1.0f) * 0.5f);
}

// --- The table --------------------------------------------------------------------------------
//
// Indexed [kind][mode], in the enumerators' own order. A static table rather than a switch because
// `function()` hands the pointer out, and a switch would need a second copy of the mapping.

constexpr Function kTable[static_cast<usize>(Kind::kCount)][static_cast<usize>(Mode::kCount)] = {
    // Linear ignores the mode: all four variants are the identity.
    {linear_in, linear_in, linear_in, linear_in},
    {sine_in, out_of<sine_in>, in_out_of<sine_in>, out_in_of<sine_in>},
    {quad_in, out_of<quad_in>, in_out_of<quad_in>, out_in_of<quad_in>},
    {cubic_in, out_of<cubic_in>, in_out_of<cubic_in>, out_in_of<cubic_in>},
    {quart_in, out_of<quart_in>, in_out_of<quart_in>, out_in_of<quart_in>},
    {quint_in, out_of<quint_in>, in_out_of<quint_in>, out_in_of<quint_in>},
    {expo_in, out_of<expo_in>, in_out_of<expo_in>, out_in_of<expo_in>},
    {circ_in, out_of<circ_in>, in_out_of<circ_in>, out_in_of<circ_in>},
    {back_in, out_of<back_in>, in_out_of<back_in>, out_in_of<back_in>},
    {elastic_in, out_of<elastic_in>, in_out_of<elastic_in>, out_in_of<elastic_in>},
    {bounce_in, out_of<bounce_in>, in_out_of<bounce_in>, out_in_of<bounce_in>},
};

constexpr const char* kKindNames[static_cast<usize>(Kind::kCount)] = {
    "linear", "sine", "quad", "cubic",   "quart", "quint",
    "expo",   "circ", "back", "elastic", "bounce"};

constexpr const char* kModeNames[static_cast<usize>(Mode::kCount)] = {"in", "out", "in-out",
                                                                      "out-in"};

}  // namespace

Function function(Kind kind, Mode mode) noexcept {
    const auto kind_index = static_cast<usize>(kind);
    const auto mode_index = static_cast<usize>(mode);
    if (kind_index >= static_cast<usize>(Kind::kCount) ||
        mode_index >= static_cast<usize>(Mode::kCount)) {
        // An out-of-range enumerator is a programmer error, and the honest response in a shipping
        // build is the identity: an animation that plays linearly is a visible oddity, where a null
        // pointer would be a crash in whatever called it.
        return linear_in;
    }
    return kTable[kind_index][mode_index];
}

f32 evaluate(Kind kind, Mode mode, f32 t) noexcept {
    return function(kind, mode)(math::saturate(t));
}

const char* kind_name(Kind kind) noexcept {
    const auto index = static_cast<usize>(kind);
    return index < static_cast<usize>(Kind::kCount) ? kKindNames[index] : "unknown";
}

const char* mode_name(Mode mode) noexcept {
    const auto index = static_cast<usize>(mode);
    return index < static_cast<usize>(Mode::kCount) ? kModeNames[index] : "unknown";
}

}  // namespace cy::ease
