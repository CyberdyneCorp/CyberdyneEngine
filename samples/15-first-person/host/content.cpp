// SPDX-License-Identifier: MIT
// content.cpp — the actions and the sounds. See content.h.

#include "content.h"

#include <cmath>
#include <numbers>
#include <utility>

#include "arena.h"
#include "play_bindings.h"

namespace sample::fps {
namespace {

using cy::f32;
using cy::u32;
using cy::Vec3;
using cy::input::Key;
using cy::play::scheme_binding;
using cy::play::stick_binding;

[[nodiscard]] cy::input::Binding control_binding(cy::input::ActionId action,
                                                 cy::input::Control control) noexcept {
    cy::input::Binding binding;
    binding.action = action;
    binding.kind = cy::input::BindingKind::Simple;
    binding.component_count = 1;
    binding.components[0].control = control;
    binding.components[0].weight = Vec3{1.0F, 0.0F, 0.0F};
    binding.trigger.kind = cy::input::TriggerKind::Down;
    return binding;
}

/// Four keys as one two-dimensional axis: x is left and right, y is back and forward.
[[nodiscard]] cy::input::Binding axis2_binding(cy::input::ActionId action, Key left, Key right,
                                               Key back, Key forward) noexcept {
    using cy::input::key_control;
    cy::input::Binding binding;
    binding.action = action;
    binding.kind = cy::input::BindingKind::Axis2D;
    binding.component_count = 4;
    binding.components[0] = {key_control(left), Vec3{-1.0F, 0.0F, 0.0F}};
    binding.components[1] = {key_control(right), Vec3{1.0F, 0.0F, 0.0F}};
    binding.components[2] = {key_control(back), Vec3{0.0F, -1.0F, 0.0F}};
    binding.components[3] = {key_control(forward), Vec3{0.0F, 1.0F, 0.0F}};
    binding.trigger.kind = cy::input::TriggerKind::Down;
    return binding;
}

[[nodiscard]] cy::Expected<cy::input::ActionId, cy::Error> declare(
    cy::input::InputServer& input, const char* name, u32 stable,
    cy::input::ActionValueType type) noexcept {
    cy::input::ActionDeclaration declaration;
    declaration.name = cy::Name::intern(name);
    declaration.stable_id = cy::input::ActionStableId{stable};
    declaration.type = type;
    return input.actions().declare(declaration);
}

/// A fixed linear congruential sequence in [−1, 1]: the same noise on every machine.
class Noise {
public:
    explicit Noise(u32 seed) noexcept : state_(seed) {}
    [[nodiscard]] f32 next() noexcept {
        state_ = (state_ * 1664525U) + 1013904223U;
        return (static_cast<f32>(state_ >> 8U) / 8388608.0F) - 1.0F;
    }

private:
    u32 state_;
};

}  // namespace

cy::Status declare_input(cy::input::InputServer& input, cy::Allocator& allocator) noexcept {
    using cy::input::ActionValueType;
    using cy::input::GamepadControl;
    using cy::input::MouseControl;
    const auto move = declare(input, kMoveAction, 1, ActionValueType::Axis2);
    const auto sprint = declare(input, kSprintAction, 2, ActionValueType::Digital);
    const auto jump = declare(input, kJumpAction, 3, ActionValueType::Digital);
    const auto crouch = declare(input, kCrouchAction, 4, ActionValueType::Digital);
    const auto fire = declare(input, kFireAction, 5, ActionValueType::Digital);
    const auto aim = declare(input, kAimAction, 6, ActionValueType::Digital);
    // Declared after the scripted run's six, so their ids are what they were.
    const auto look = declare(input, kLookAction, 7, ActionValueType::Axis2);
    const auto reload = declare(input, kReloadAction, 8, ActionValueType::Digital);
    if (!move || !sprint || !jump || !crouch || !fire || !aim || !look || !reload) {
        return cy::fail(cy::ErrorCode::Internal, "an fps action could not be declared");
    }
    if (cy::Status finalized = input.finalize_declarations(); !finalized) {
        return finalized;
    }
    cy::input::MappingContext context(allocator);
    context.set_name(cy::Name::intern("fps"));
    const cy::input::Binding bindings[] = {
        axis2_binding(*move, Key::A, Key::D, Key::S, Key::W),
        control_binding(*sprint, cy::input::key_control(Key::LeftShift)),
        control_binding(*jump, cy::input::key_control(Key::Space)),
        control_binding(*crouch, cy::input::key_control(Key::C)),
        control_binding(*fire, cy::input::mouse_control(MouseControl::Left)),
        control_binding(*aim, cy::input::mouse_control(MouseControl::Right)),
        // THE PLAYED GAME'S: a second crouch key, the reload, and the pad — sticks with radial
        // dead zones, the triggers past half way, the face buttons. A player rebinds any of
        // these (samples/play/bindings_file.h); the six above are the scripted run's as they were.
        scheme_binding(*crouch, cy::input::key_control(Key::LeftControl)),
        scheme_binding(*reload, cy::input::key_control(Key::R)),
        stick_binding(*move, GamepadControl::LeftStickX, GamepadControl::LeftStickY),
        stick_binding(*look, GamepadControl::RightStickX, GamepadControl::RightStickY),
        scheme_binding(*sprint, cy::input::gamepad_control(GamepadControl::LeftStick)),
        scheme_binding(*jump, cy::input::gamepad_control(GamepadControl::South)),
        scheme_binding(*crouch, cy::input::gamepad_control(GamepadControl::East)),
        scheme_binding(*reload, cy::input::gamepad_control(GamepadControl::West)),
        scheme_binding(*fire, cy::input::gamepad_control(GamepadControl::RightTrigger)),
        scheme_binding(*aim, cy::input::gamepad_control(GamepadControl::LeftTrigger)),
    };
    for (const cy::input::Binding& binding : bindings) {
        if (cy::Status added = context.add(binding); !added) {
            return added;
        }
    }
    const auto registered = input.register_context(std::move(context));
    if (!registered) {
        return cy::make_unexpected(registered.error());
    }
    const auto pushed = input.user(0).push_context(*registered, 0, cy::input::FocusLayer::Gameplay);
    if (!pushed) {
        return cy::make_unexpected(pushed.error());
    }
    return cy::ok();
}

const char* sound_cue(Sound sound) noexcept {
    switch (sound) {
        case Sound::Shot:
            return kShotCue;
        case Sound::Impact:
            return kImpactCue;
        case Sound::Hit:
            return kHitCue;
        case Sound::Down:
        case Sound::Count:
            break;
    }
    return kDownCue;
}

cy::Status make_sound(Sound sound, u32 sample_rate, cy::Array<f32>& out) noexcept {
    struct Recipe {
        f32 seconds;
        /// Noise: its level and how many times a second its envelope falls by e.
        f32 noise;
        f32 noise_decay;
        /// A tone: its level, its frequency at the start and at the end, and its decay.
        f32 tone;
        f32 hz_from;
        f32 hz_to;
        f32 tone_decay;
    };
    static constexpr Recipe kRecipes[] = {
        {0.45F, 0.9F, 18.0F, 0.6F, 95.0F, 55.0F, 9.0F},      // the shot: a crack over a thump
        {0.12F, 0.5F, 45.0F, 0.0F, 0.0F, 0.0F, 1.0F},        // the impact: a tick
        {0.20F, 0.15F, 30.0F, 0.7F, 180.0F, 140.0F, 16.0F},  // the hit: a knock
        {0.60F, 0.05F, 10.0F, 0.6F, 330.0F, 110.0F, 4.0F},   // down: a falling tone
    };
    const Recipe& recipe = kRecipes[static_cast<u32>(sound)];
    const auto frames = static_cast<u32>(recipe.seconds * static_cast<f32>(sample_rate));
    if (cy::Status sized = out.resize(frames); !sized) {
        return sized;
    }
    Noise noise(0x5EED0015U + static_cast<u32>(sound));
    const f32 rate = static_cast<f32>(sample_rate);
    f32 phase = 0.0F;
    for (u32 frame = 0; frame < frames; ++frame) {
        const f32 time = static_cast<f32>(frame) / rate;
        const f32 along = time / recipe.seconds;
        const f32 hz = recipe.hz_from + ((recipe.hz_to - recipe.hz_from) * along);
        phase += 2.0F * std::numbers::pi_v<f32> * hz / rate;
        const f32 crack = recipe.noise * noise.next() * std::exp(-recipe.noise_decay * time);
        const f32 body = recipe.tone * std::sin(phase) * std::exp(-recipe.tone_decay * time);
        out[frame] = 0.5F * (crack + body);
    }
    return cy::ok();
}

}  // namespace sample::fps
