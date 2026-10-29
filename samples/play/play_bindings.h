// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_PLAY_PLAY_BINDINGS_H
#define CY_SAMPLE_PLAY_PLAY_BINDINGS_H
// play_bindings.h — the bindings a played game adds to its scripted run's: a control in one
// scheme, and a stick. Header-only and over the input server alone, so a game host declares them
// whether or not the play host is in the build.
//
// THE SCRIPTED RUN'S BINDINGS COME FIRST AND KEEP THEIR INDICES: the input layer's arbitration
// breaks a tie between two bindings of one action by the lower index, so an idle pad bound after
// the keys never takes a value from them, and a scripted run resolves exactly as it did.

#include <cy/core/math/vec.h>
#include <cy/servers/input/binding.h>
#include <cy/servers/input/types.h>

namespace cy::play {

/// The radial dead zone a stick is read through, and the deflection past it that counts as moved.
inline constexpr f32 kStickDeadZone = 0.15F;
inline constexpr f32 kStickActuation = 0.001F;

/// One control, in the scheme its device belongs to: rebinding keeps a player's pad and keyboard
/// choices apart by it.
[[nodiscard]] inline input::Binding scheme_binding(input::ActionId action,
                                                   input::Control control) noexcept {
    input::Binding binding;
    binding.action = action;
    binding.kind = input::BindingKind::Simple;
    binding.schemes = input::scheme_mask(input::scheme_of(control.kind));
    binding.component_count = 1;
    binding.components[0].control = control;
    binding.components[0].weight = Vec3{1.0F, 0.0F, 0.0F};
    binding.trigger.kind = input::TriggerKind::Down;
    return binding;
}

/// A stick as one two-dimensional axis, its dead zone RADIAL — an axis-wise one leaves a square
/// hole and makes the diagonals easier to reach than the cardinals — and rescaled, so the stick
/// still reads one at full deflection. Any deflection past the dead zone is the stick's: at the
/// default half-way threshold, a stick eased forward would lose the arbitration to idle keys.
[[nodiscard]] inline input::Binding stick_binding(input::ActionId action, input::GamepadControl x,
                                                  input::GamepadControl y) noexcept {
    input::Binding binding;
    binding.action = action;
    binding.kind = input::BindingKind::Radial;
    binding.schemes = input::scheme_mask(input::SchemeKind::Gamepad);
    binding.component_count = 2;
    binding.components[0] = {input::gamepad_control(x), Vec3{1.0F, 0.0F, 0.0F}};
    binding.components[1] = {input::gamepad_control(y), Vec3{0.0F, 1.0F, 0.0F}};
    binding.processor_count = 1;
    binding.processors[0] =
        input::Processor{input::ProcessorKind::RadialDeadZone, kStickDeadZone, 0.0F};
    binding.trigger.kind = input::TriggerKind::Down;
    binding.trigger.actuation_threshold = kStickActuation;
    return binding;
}

}  // namespace cy::play

#endif  // CY_SAMPLE_PLAY_PLAY_BINDINGS_H
