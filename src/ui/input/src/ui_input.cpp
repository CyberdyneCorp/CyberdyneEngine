// SPDX-License-Identifier: MIT
// The UI actions, their default bindings, and the bridge from the input server to the toolkit.
// See ui_input.h.

#include <cy/ui/input/ui_input.h>

namespace cy::ui {
namespace {

using input::BindingKind;
using input::Control;
using input::GamepadControl;
using input::Key;

[[nodiscard]] input::ActionId id_of(const UiInputActions& actions, UiAction action) noexcept {
    return actions.ids[static_cast<u32>(action)];
}

/// One control: the action is down while it is.
[[nodiscard]] input::Binding simple(input::ActionId action, Control control) noexcept {
    input::Binding binding;
    binding.action = action;
    binding.kind = BindingKind::Simple;
    binding.component_count = 1;
    binding.components[0].control = control;
    binding.components[0].weight = Vec3{1.0F, 0.0F, 0.0F};
    return binding;
}

/// One half of a stick's axis: down while the stick is pushed past half way in that direction.
[[nodiscard]] input::Binding half_axis(input::ActionId action, GamepadControl axis,
                                       f32 direction) noexcept {
    input::Binding binding = simple(action, input::gamepad_control(axis));
    binding.components[0].weight = Vec3{direction, 0.0F, 0.0F};
    binding.processor_count = 1;
    binding.processors[0] = input::Processor{input::ProcessorKind::Clamp, 0.0F, 1.0F};
    return binding;
}

/// Two keys held together.
[[nodiscard]] input::Binding chord(input::ActionId action, Key held, Key pressed) noexcept {
    input::Binding binding;
    binding.action = action;
    binding.kind = BindingKind::Chord;
    binding.component_count = 2;
    binding.components[0].control = input::key_control(held);
    binding.components[1].control = input::key_control(pressed);
    binding.trigger.kind = input::TriggerKind::Chord;
    return binding;
}

}  // namespace

Status declare_ui_actions(input::ActionRegistry& registry, UiInputActions& out) noexcept {
    for (u32 index = 0; index < static_cast<u32>(UiAction::Count); ++index) {
        input::ActionDeclaration declaration;
        declaration.name = Name::intern(ui_action_name(static_cast<UiAction>(index)));
        declaration.stable_id = input::ActionStableId{kUiActionStableBase + index};
        declaration.type = input::ActionValueType::Digital;
        Expected<input::ActionId, Error> declared = registry.declare(declaration);
        if (!declared) {
            return make_unexpected(declared.error());
        }
        out.ids[index] = *declared;
    }
    return ok();
}

Expected<input::MappingContext, Error> make_ui_context(Allocator& allocator,
                                                       const UiInputActions& actions) noexcept {
    input::MappingContext context(allocator);
    context.set_name(Name::intern("UI"));
    const auto key = [](Key value) noexcept { return input::key_control(value); };
    const auto pad = [](GamepadControl value) noexcept { return input::gamepad_control(value); };
    const input::Binding bindings[] = {
        // Enter, not Space: Space is text in a text field, and the field has the focus then.
        simple(id_of(actions, UiAction::Accept), key(Key::Enter)),
        simple(id_of(actions, UiAction::Accept), pad(GamepadControl::South)),
        simple(id_of(actions, UiAction::Cancel), key(Key::Escape)),
        simple(id_of(actions, UiAction::Cancel), pad(GamepadControl::East)),
        simple(id_of(actions, UiAction::NavigateUp), key(Key::Up)),
        simple(id_of(actions, UiAction::NavigateUp), pad(GamepadControl::DpadUp)),
        half_axis(id_of(actions, UiAction::NavigateUp), GamepadControl::LeftStickY, 1.0F),
        simple(id_of(actions, UiAction::NavigateDown), key(Key::Down)),
        simple(id_of(actions, UiAction::NavigateDown), pad(GamepadControl::DpadDown)),
        half_axis(id_of(actions, UiAction::NavigateDown), GamepadControl::LeftStickY, -1.0F),
        simple(id_of(actions, UiAction::NavigateLeft), key(Key::Left)),
        simple(id_of(actions, UiAction::NavigateLeft), pad(GamepadControl::DpadLeft)),
        half_axis(id_of(actions, UiAction::NavigateLeft), GamepadControl::LeftStickX, -1.0F),
        simple(id_of(actions, UiAction::NavigateRight), key(Key::Right)),
        simple(id_of(actions, UiAction::NavigateRight), pad(GamepadControl::DpadRight)),
        half_axis(id_of(actions, UiAction::NavigateRight), GamepadControl::LeftStickX, 1.0F),
        simple(id_of(actions, UiAction::NextTab), pad(GamepadControl::RightShoulder)),
        chord(id_of(actions, UiAction::NextTab), Key::LeftControl, Key::Tab),
        simple(id_of(actions, UiAction::PreviousTab), pad(GamepadControl::LeftShoulder)),
        simple(id_of(actions, UiAction::Context), pad(GamepadControl::North)),
        half_axis(id_of(actions, UiAction::ScrollUp), GamepadControl::RightStickY, 1.0F),
        half_axis(id_of(actions, UiAction::ScrollDown), GamepadControl::RightStickY, -1.0F),
        simple(id_of(actions, UiAction::FocusNext), key(Key::Tab)),
        chord(id_of(actions, UiAction::FocusPrevious), Key::LeftShift, Key::Tab),
    };
    for (const input::Binding& binding : bindings) {
        if (Status added = context.add(binding); !added) {
            return make_unexpected(added.error());
        }
    }
    return context;
}

namespace {

/// The glyph of a control by scheme: a gamepad's, a touch screen's, or a keyboard and mouse's.
[[nodiscard]] const char* by_scheme(input::SchemeKind scheme, const char* pad, const char* touch,
                                    const char* keys) noexcept {
    if (scheme == input::SchemeKind::Gamepad) {
        return pad;
    }
    return scheme == input::SchemeKind::Touch ? touch : keys;
}

}  // namespace

const char* ui_action_glyph(UiAction action, input::SchemeKind scheme) noexcept {
    const bool pad = scheme == input::SchemeKind::Gamepad;
    switch (action) {
        case UiAction::Accept:
            return by_scheme(scheme, "A", "Tap", "Enter");
        case UiAction::Cancel:
            return by_scheme(scheme, "B", "Back", "Esc");
        case UiAction::NavigateUp:
            return pad ? "D-pad Up" : "Up";
        case UiAction::NavigateDown:
            return pad ? "D-pad Down" : "Down";
        case UiAction::NavigateLeft:
            return pad ? "D-pad Left" : "Left";
        case UiAction::NavigateRight:
            return pad ? "D-pad Right" : "Right";
        case UiAction::NextTab:
            return pad ? "RB" : "Ctrl+Tab";
        case UiAction::PreviousTab:
            return pad ? "LB" : "Ctrl+Shift+Tab";
        case UiAction::Context:
            return by_scheme(scheme, "Y", "Hold", "Right click");
        case UiAction::ScrollUp:
            return pad ? "Right stick up" : "Wheel up";
        case UiAction::ScrollDown:
            return pad ? "Right stick down" : "Wheel down";
        case UiAction::FocusNext:
            return pad ? "D-pad" : "Tab";
        case UiAction::FocusPrevious:
            return pad ? "D-pad" : "Shift+Tab";
        case UiAction::Count:
            break;
    }
    return "";
}

UiInputBridge::UiInputBridge(Toolkit& toolkit, input::InputServer& server, u32 user) noexcept
    : toolkit_(&toolkit), server_(&server), user_(user) {}

Status UiInputBridge::start(const UiInputActions& actions, input::ContextHandle context,
                            i32 priority) noexcept {
    actions_ = actions;
    context_ = context;
    // AT THE INTERFACE'S LAYER, without pass-through: these bindings act only while the user's
    // focus is the interface, and the game's act only while it is not.
    Expected<u32, Error> pushed =
        server_->user(user_).push_context(context, priority, input::FocusLayer::Interface, false);
    return pushed ? ok() : Status{make_unexpected(pushed.error())};
}

void UiInputBridge::stop() noexcept {
    (void)server_->user(user_).pop_context(context_);
    server_->user(user_).set_focus(input::FocusLayer::Gameplay);
    server_->user(user_).set_text_entry_active(false);
}

input::SchemeKind UiInputBridge::scheme() const noexcept {
    return server_->user(user_).scheme().active();
}

Status UiInputBridge::pump(UiInputReport* report) noexcept {
    UiInputReport local;
    input::InputUser& user = server_->user(user_);
    bool fired[static_cast<u32>(UiAction::Count)] = {};
    for (u32 index = 0; index < static_cast<u32>(UiAction::Count); ++index) {
        const input::ActionId id = actions_.ids[index];
        fired[index] = id < user.action_count() && user.action_state(id).just_pressed();
    }
    // A CHORD ALSO PRESSES ITS LAST KEY: Shift+Tab is Tab too, and Ctrl+Tab is Tab too. The chord
    // is what the player meant.
    const auto at = [](UiAction action) noexcept { return static_cast<u32>(action); };
    if (fired[at(UiAction::FocusPrevious)] || fired[at(UiAction::NextTab)]) {
        fired[at(UiAction::FocusNext)] = false;
    }
    const bool typing = toolkit_->wants_text_input();
    for (u32 index = 0; index < static_cast<u32>(UiAction::Count); ++index) {
        if (!fired[index]) {
            continue;
        }
        const auto action = static_cast<UiAction>(index);
        // A text field keeps its own left and right — the caret's, through `TextCommand`s.
        if (typing && (action == UiAction::NavigateLeft || action == UiAction::NavigateRight)) {
            continue;
        }
        ++local.fired;
        local.consumed += toolkit_->action(action) ? 1U : 0U;
    }
    // THE FOCUS LAYER FOLLOWS THE TOOLKIT, for the next tick's resolution: the interface's while a
    // modal, a drop-down or a text field holds the input, or while a menu screen is up; the
    // game's otherwise.
    const bool interface = menu_mode_ || toolkit_->captures_input();
    local.focus = interface ? input::FocusLayer::Interface : input::FocusLayer::Gameplay;
    if (user.focus() != local.focus) {
        user.set_focus(local.focus);
    }
    if (user.text_entry_active() != toolkit_->wants_text_input()) {
        user.set_text_entry_active(toolkit_->wants_text_input());
    }
    if (report != nullptr) {
        *report = local;
    }
    return ok();
}

}  // namespace cy::ui
