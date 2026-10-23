// SPDX-License-Identifier: MIT
#ifndef CY_UI_TOOLKIT_ANIMATION_H
#define CY_UI_TOOLKIT_ANIMATION_H
// Easing, and what one interface animation is. Issue #91, stage 5.
//
// `ui-system`: "UI animation SHALL be internal to the UI system, not delegated to the general
// tween system, so transitions run without crossing the scripting boundary per element per frame
// and without coupling UI lifetime to entity lifetime." An `Animation` is therefore a value the
// toolkit advances in its own frame: a game (or a Swift module, through one ABI call) starts it and
// is not called again until it is over.
//
// "Animations affecting only paint properties SHALL NOT trigger layout." Every property here is a
// paint property — opacity, the render transform (`PaintData::translate`, `scale`) and the
// background — so a fade, a slide and a pop dirty paint and nothing else, and `LayoutReport::
// measured` is zero on every frame of them. A slide is the render transform moving the subtree,
// not its anchors.

#include <cy/core/base/types.h>

#include <string_view>

namespace cy::ui {

/// How time becomes progress.
enum class Easing : u8 {
    Linear = 0,
    /// CSS's `ease-in`, `ease-out` and `ease-in-out`, as cubic polynomials: smooth at the ends that
    /// are named.
    EaseIn,
    EaseOut,
    EaseInOut,
    /// CSS's default `ease`: a quick start that settles.
    Ease,
    /// A critically under-damped spring: past the target and back, settling exactly at one. The
    /// "spring dynamics" `ui-system` asks for, closed-form so a frame's time step does not change
    /// the curve.
    Spring,
    /// Overshoots and settles, as a pop-in wants.
    Back,
    Count,
};

[[nodiscard]] const char* easing_name(Easing easing) noexcept;
/// The easing a `.cyss` `transition-timing-function` keyword names, or `Ease` for one it does not.
[[nodiscard]] Easing easing_from_name(std::string_view name) noexcept;
/// Progress at `t` in [0, 1], clamped. Zero at zero and one at one for every easing.
[[nodiscard]] f32 ease(Easing easing, f32 t) noexcept;

/// What an animation drives. Paint properties only — see the header.
enum class AnimatedProperty : u8 {
    Opacity = 0,
    TranslateX,
    TranslateY,
    Scale,
    /// `PaintData::background`, interpolated per premultiplied channel.
    Background,
    Count,
};

[[nodiscard]] const char* animated_property_name(AnimatedProperty property) noexcept;

/// One animation of one property of one element.
struct Animation {
    AnimatedProperty property = AnimatedProperty::Opacity;
    /// Numbers for every property but `Background`, which uses the colours.
    f32 from = 0.0F;
    f32 to = 1.0F;
    u32 from_colour = 0;
    u32 to_colour = 0;
    /// Seconds.
    f32 duration = 0.25F;
    f32 delay = 0.0F;
    Easing easing = Easing::EaseOut;
    /// DECORATIVE: with the reduced-motion preference on, it jumps to its end on its first frame.
    /// State feedback — a pressed button darkening — is not decorative and still plays, which is
    /// `ui-system`'s "decorative animations SHALL be shortened or disabled while state feedback
    /// remains".
    bool decorative = true;
};

/// Linear interpolation of a premultiplied 0xAARRGGBB colour, channel by channel, rounded.
[[nodiscard]] u32 mix_colour(u32 from, u32 to, f32 t) noexcept;

}  // namespace cy::ui

#endif  // CY_UI_TOOLKIT_ANIMATION_H
