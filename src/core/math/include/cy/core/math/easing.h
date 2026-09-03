#pragma once
// The shared easing table. Task 3.1.5.
//
// `core-math` — "Curves and easing": a shared easing function table covering linear, sine, quad,
// cubic, quart, quint, expo, circ, back, elastic and bounce, each with in, out, in-out and out-in
// variants.
//
// **Shared** is the requirement that matters. An engine accumulates half a dozen private copies of
// these — one in the UI, one in the animation system, one in a tween helper — and they disagree at
// the edges, so a designer who matches a UI fade to an animation curve by eye gets two different
// motions. There is one table, it is here, and the four modes are derived from the `In` form by the
// three standard reflections rather than written out, so a mode cannot disagree with its own
// family.

#include <cy/core/base/types.h>

namespace cy::ease {

/// The eleven families. `Linear` ignores the mode: all four of its variants are the identity, which
/// is stated here so nobody adds a special case for it later.
enum class Kind : u32 {
    Linear = 0,
    Sine,
    Quad,
    Cubic,
    Quart,
    Quint,
    Expo,
    Circ,
    Back,
    Elastic,
    Bounce,
    kCount,
};

/// `In` accelerates from rest, `Out` decelerates to rest, `InOut` does both with the `In` half
/// first, and `OutIn` does both with the `Out` half first.
enum class Mode : u32 {
    In = 0,
    Out,
    InOut,
    OutIn,
    kCount,
};

/// A normalised easing function: takes `t` in [0, 1] and returns the eased fraction.
///
/// The return value is **not** confined to [0, 1]. `Back` and `Elastic` overshoot deliberately, and
/// clamping them here would silently remove the effect they exist for. A caller that cannot accept
/// an overshoot picks a family that does not have one.
using Function = f32 (*)(f32);

/// Evaluate directly. `t` is clamped to [0, 1] first, so an extrapolated parameter does not produce
/// an undefined tail from `Expo` or `Elastic`.
[[nodiscard]] f32 evaluate(Kind kind, Mode mode, f32 t) noexcept;

/// The function itself, for a caller that evaluates the same curve many times — a keyframe track, a
/// particle module — and would rather not re-dispatch per sample.
[[nodiscard]] Function function(Kind kind, Mode mode) noexcept;

/// The enumerator's own spelling, for an editor drop-down and for a diagnostic. Never null.
[[nodiscard]] const char* kind_name(Kind kind) noexcept;
[[nodiscard]] const char* mode_name(Mode mode) noexcept;

}  // namespace cy::ease
