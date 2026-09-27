// SPDX-License-Identifier: MIT
// The `input` adapter: `cy::abi::game::InputBackend` over `cy::input::InputServer`.
// `add-swift-game-api`.
//
// OWNER: implementer A. The header says what each answer is read from and why; this file is the
// mapping. Every domain failure is reported through `cy::abi::report` with a message naming what
// was missing, and returned — the thunk in front of it has already checked everything else.

#include <cy/abi/errors.h>
#include <cy/game_backend/input_backend.h>

#include <cstdio>
#include <string_view>

namespace cy::game_backend {
namespace {

using cy::input::ActionFlag;
using cy::input::DeviceKind;
using cy::input::DeviceRecord;
using cy::input::InputServer;
using cy::input::Key;
using cy::input::MouseControl;

/// NOT_FOUND, with a message that quotes the name nothing answered to.
[[nodiscard]] CyResult not_found(const char* what, const char* name) noexcept {
    char message[256];
    std::snprintf(message, sizeof(message), "%s '%s'", what, name);
    return cy::abi::report(CY_RESULT_NOT_FOUND, message);
}

/// The connected device of `kind` the server routes to `user`, or null. Under the shared
/// keyboard-and-mouse policy one device reaches every user, so it is every user's.
[[nodiscard]] const DeviceRecord* routed_device(const InputServer& server, DeviceKind kind,
                                                u32 user) noexcept {
    const auto& devices = server.devices();
    for (u32 index = 0; index < devices.count(); ++index) {
        const DeviceRecord& record = devices.at(index);
        if (!record.connected || record.description.kind != kind) {
            continue;
        }
        bool shared = false;
        const u32 routed = devices.route(record.id, shared);
        if (shared || routed == user) {
            return &record;
        }
    }
    return nullptr;
}

[[nodiscard]] bool held(const DeviceRecord& record, u16 code) noexcept {
    return record.controls[code] >= 0.5F;
}

[[nodiscard]] bool held(const DeviceRecord& record, Key key) noexcept {
    return held(record, static_cast<u16>(key));
}

/// The CY_INPUT_BUTTON_* bit of a mouse control, or zero for a control that is not a button.
/// `MouseControl::Left` to `Extra2` are contiguous and in the C header's bit order.
[[nodiscard]] u32 button_bit(u16 code) noexcept {
    const auto first = static_cast<u16>(MouseControl::Left);
    const auto last = static_cast<u16>(MouseControl::Extra2);
    if (code < first || code > last) {
        return 0U;
    }
    return 1U << static_cast<u32>(code - first);
}
static_assert(CY_INPUT_BUTTON_LEFT == 1U << 0U && CY_INPUT_BUTTON_RIGHT == 1U << 1U &&
                  CY_INPUT_BUTTON_MIDDLE == 1U << 2U && CY_INPUT_BUTTON_EXTRA1 == 1U << 3U &&
                  CY_INPUT_BUTTON_EXTRA2 == 1U << 4U,
              "the button bits follow MouseControl::Left..Extra2");

/// Every button the mouse holds now, as CY_INPUT_BUTTON_* bits.
[[nodiscard]] u32 buttons_held(const DeviceRecord& mouse) noexcept {
    u32 buttons = 0;
    for (auto code = static_cast<u16>(MouseControl::Left);
         code <= static_cast<u16>(MouseControl::Extra2); ++code) {
        if (held(mouse, code)) {
            buttons |= button_bit(code);
        }
    }
    return buttons;
}

/// The engine's action flags as the C header spells them.
[[nodiscard]] u32 abi_action_flags(u16 flags) noexcept {
    u32 out = 0;
    out |= cy::input::has_flag(flags, ActionFlag::Pressed) ? CY_INPUT_ACTION_PRESSED : 0U;
    out |= cy::input::has_flag(flags, ActionFlag::JustPressed) ? CY_INPUT_ACTION_JUST_PRESSED : 0U;
    out |=
        cy::input::has_flag(flags, ActionFlag::JustReleased) ? CY_INPUT_ACTION_JUST_RELEASED : 0U;
    out |= cy::input::has_flag(flags, ActionFlag::Triggered) ? CY_INPUT_ACTION_TRIGGERED : 0U;
    out |= cy::input::has_flag(flags, ActionFlag::Synthetic) ? CY_INPUT_ACTION_SYNTHETIC : 0U;
    return out;
}

}  // namespace

// --- Pointer edges ----------------------------------------------------------------------------

void InputAdapter::observe_pending() noexcept {
    const auto& pending = server_->pending();
    const auto& devices = server_->devices();
    // In buffer order, which is resolution order for a single backend; `resolve_tick` sorts only
    // when two producers interleaved.
    for (usize index = 0; index < pending.size(); ++index) {
        const cy::input::DeviceEvent& event = pending[index];
        const DeviceRecord* record = devices.find(event.device);
        if (record == nullptr || !record->connected ||
            record->description.kind != DeviceKind::Mouse) {
            continue;
        }
        bool shared = false;
        const u32 routed = devices.route(event.device, shared);
        for (u32 user = 0; user < kMaxPointerUsers; ++user) {
            if (shared || routed == user) {
                observe(event, user);
            }
        }
    }
}

void InputAdapter::observe(const cy::input::DeviceEvent& event, u32 user) noexcept {
    PointerTrack& track = tracks_[user];
    if (!track.level_known) {
        // The device state is still the previous tick's here, which is the level these events
        // start from.
        const DeviceRecord* mouse = server_->devices().find(event.device);
        track.level = (mouse != nullptr) ? buttons_held(*mouse) : 0U;
        track.level_known = true;
    }
    PointerEdges& edges = track.accumulating;
    const u16 code = event.control.code;
    if (const u32 bit = button_bit(code); bit != 0U) {
        const bool down = event.value >= 0.5F;
        if (down && (track.level & bit) == 0U) {
            edges.pressed |= bit;
            track.level |= bit;
        } else if (!down && (track.level & bit) != 0U) {
            edges.released |= bit;
            track.level &= ~bit;
        }
        return;
    }
    switch (static_cast<MouseControl>(code)) {
        case MouseControl::MoveX:
            edges.delta[0] += event.value;
            break;
        case MouseControl::MoveY:
            edges.delta[1] += event.value;
            break;
        case MouseControl::WheelX:
            edges.wheel[0] += event.value;
            break;
        case MouseControl::Wheel:
            edges.wheel[1] += event.value;
            break;
        default:
            break;
    }
}

void InputAdapter::begin_frame() noexcept {
    for (PointerTrack& track : tracks_) {
        track.published = track.accumulating;
        track.accumulating = PointerEdges{};
    }
}

void InputAdapter::set_pointer_focus(u32 user, bool in_window, bool over_ui) noexcept {
    if (user < kMaxPointerUsers) {
        tracks_[user].in_window = in_window;
        tracks_[user].over_ui = over_ui;
    }
}

// --- InputBackend ------------------------------------------------------------------------------

CyResult InputAdapter::check_user(u32 user) const noexcept {
    if (user >= server_->user_count()) {
        return cy::abi::report(CY_RESULT_OUT_OF_RANGE,
                               "input: the input server has no user with that index");
    }
    return CY_RESULT_OK;
}

CyResult InputAdapter::find_action(const char* name, CyInputAction& out_action) noexcept {
    const cy::Name key = cy::Name::find(std::string_view(name));
    const cy::input::ActionId action =
        key.is_empty() ? cy::input::kInvalidAction : server_->actions().find(key);
    if (action == cy::input::kInvalidAction) {
        return not_found("input: no action is declared as", name);
    }
    out_action = action;
    return CY_RESULT_OK;
}

CyResult InputAdapter::action_state(u32 user, CyInputAction action,
                                    CyInputActionState& out_state) noexcept {
    if (const CyResult checked = check_user(user); checked != CY_RESULT_OK) {
        return checked;
    }
    const cy::input::InputUser& target = server_->user(user);
    if (action >= target.action_count()) {
        return cy::abi::report(CY_RESULT_NOT_FOUND, "input: no action has that index");
    }
    const cy::input::ActionState& state = target.action_state(action);
    out_state.flags = abi_action_flags(state.flags);
    out_state.value[0] = state.value.axis.x;
    out_state.value[1] = state.value.axis.y;
    out_state.value[2] = state.value.axis.z;
    out_state.press_count = state.press_count;
    out_state.release_count = state.release_count;
    out_state.tick = server_->tick();
    return CY_RESULT_OK;
}

CyResult InputAdapter::pointer(u32 user, CyInputPointer& out_pointer) noexcept {
    if (const CyResult checked = check_user(user); checked != CY_RESULT_OK) {
        return checked;
    }
    const DeviceRecord* mouse = routed_device(*server_, DeviceKind::Mouse, user);
    if (mouse == nullptr) {
        return CY_RESULT_OK;  // No pointing device: PRESENT clear, everything else zero.
    }
    out_pointer.flags = CY_INPUT_POINTER_PRESENT;
    out_pointer.buttons = buttons_held(*mouse);
    out_pointer.position[0] = mouse->controls[static_cast<u16>(MouseControl::PositionX)];
    out_pointer.position[1] = mouse->controls[static_cast<u16>(MouseControl::PositionY)];
    if (user >= kMaxPointerUsers) {
        out_pointer.flags |= CY_INPUT_POINTER_IN_WINDOW;
        return CY_RESULT_OK;
    }
    const PointerTrack& track = tracks_[user];
    out_pointer.flags |= track.in_window ? CY_INPUT_POINTER_IN_WINDOW : 0U;
    out_pointer.flags |= track.over_ui ? CY_INPUT_POINTER_OVER_UI : 0U;
    out_pointer.buttons_pressed = track.published.pressed;
    out_pointer.buttons_released = track.published.released;
    out_pointer.delta[0] = track.published.delta[0];
    out_pointer.delta[1] = track.published.delta[1];
    out_pointer.wheel[0] = track.published.wheel[0];
    out_pointer.wheel[1] = track.published.wheel[1];
    return CY_RESULT_OK;
}

CyResult InputAdapter::modifiers(u32 user, u32& out_modifiers) noexcept {
    if (const CyResult checked = check_user(user); checked != CY_RESULT_OK) {
        return checked;
    }
    out_modifiers = 0;
    const DeviceRecord* keyboard = routed_device(*server_, DeviceKind::Keyboard, user);
    if (keyboard == nullptr) {
        return CY_RESULT_OK;
    }
    // The engine's keyboard numbering has no Super key, so CY_INPUT_MOD_SUPER is never set here.
    if (held(*keyboard, Key::LeftShift) || held(*keyboard, Key::RightShift)) {
        out_modifiers |= CY_INPUT_MOD_SHIFT;
    }
    if (held(*keyboard, Key::LeftControl) || held(*keyboard, Key::RightControl)) {
        out_modifiers |= CY_INPUT_MOD_CTRL;
    }
    if (held(*keyboard, Key::LeftAlt) || held(*keyboard, Key::RightAlt)) {
        out_modifiers |= CY_INPUT_MOD_ALT;
    }
    return CY_RESULT_OK;
}

const cy::input::MappingContext* InputAdapter::resolve_context(
    CyInputContext context) const noexcept {
    // The server issues every context at generation 1 and never frees one (server.cpp), so any
    // other generation is a value it did not hand out.
    const auto handle = cy::input::ContextHandle::from_bits(context);
    if (handle.generation() != 1U) {
        return nullptr;
    }
    return server_->context(handle);
}

CyResult InputAdapter::find_context(const char* name, CyInputContext& out_context) noexcept {
    const cy::Name key = cy::Name::find(std::string_view(name));
    if (!key.is_empty()) {
        for (u32 index = 0;; ++index) {
            const auto handle = cy::input::ContextHandle::from_slot(index, 1);
            const cy::input::MappingContext* context = server_->context(handle);
            if (context == nullptr) {
                break;
            }
            if (context->name() == key) {
                out_context = handle.bits();
                return CY_RESULT_OK;
            }
        }
    }
    return not_found("input: no mapping context is registered as", name);
}

CyResult InputAdapter::push_context(u32 user, CyInputContext context, i32 priority) noexcept {
    if (const CyResult checked = check_user(user); checked != CY_RESULT_OK) {
        return checked;
    }
    if (resolve_context(context) == nullptr) {
        return cy::abi::report(CY_RESULT_NOT_FOUND, "input: that mapping context does not exist");
    }
    // The user refuses a context already on its stack (ALREADY_EXISTS) and a full stack
    // (OUT_OF_RANGE); `ErrorCode` and `CyResult` share those values, so its error is the answer.
    const auto handle = cy::input::ContextHandle::from_bits(context);
    if (auto pushed = server_->user(user).push_context(handle, priority); !pushed) {
        return cy::abi::report(pushed.error());
    }
    return CY_RESULT_OK;
}

CyResult InputAdapter::pop_context(u32 user, CyInputContext context) noexcept {
    if (const CyResult checked = check_user(user); checked != CY_RESULT_OK) {
        return checked;
    }
    if (!server_->user(user).pop_context(cy::input::ContextHandle::from_bits(context))) {
        return cy::abi::report(CY_RESULT_NOT_FOUND,
                               "input: that mapping context is not on the user's stack");
    }
    return CY_RESULT_OK;
}

void bind(cy::abi::Host& host, InputAdapter* adapter) noexcept {
    host.game.input = adapter;
}

}  // namespace cy::game_backend
